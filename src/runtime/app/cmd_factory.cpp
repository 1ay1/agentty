#include "agentty/runtime/app/cmd_factory.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "agentty/auth/auth.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace agentty::app::cmd {

using maya::Cmd;

namespace {

// ── Salvaged-call re-leak dedup ──────────────────────────────────────────────
// Weak local models on the OpenAI-compat path (qwen2.5-coder:7b via Ollama,
// llama.cpp templates) leak tool calls as bare JSON in the `content` channel
// instead of the structured tool_calls[] channel. The transport salvages those
// into real tool calls with a synthetic `call_salvaged_N` id (see
// provider/openai/transport.cpp). But these models routinely RE-LEAK the same
// call on the post-tool sub-turn: they see the tool_result, then emit the
// identical {"name":...,"arguments":...} again. Without a guard that re-leaked
// call runs a SECOND time (the duplicate stuck card the user reported, e.g. a
// `remember` that ran DONE then a clone stuck RUNNING).
//
// A structured tool call is the model's deliberate intent (calling `read`
// twice with the same path is legitimate), so we ONLY dedup SALVAGED calls —
// identified by the `call_salvaged_` id prefix, which no real server ever
// mints. The scope is the current agent turn: the run of consecutive Assistant
// messages since the last User message. If a pending salvaged call is
// byte-identical (same name + same args) to a tool call already TERMINAL in
// that run, it's a re-leak — we resolve it as Failed-without-side-effects
// (never re-execute) so the wire-layer tool_use ↔ tool_result pairing stays
// valid while the side effect happens exactly once.
[[nodiscard]] bool is_salvaged_call(const ToolCallId& id) noexcept {
    return std::string_view{id.value}.starts_with("call_salvaged_");
}

} // namespace

// Resolve every PENDING salvaged call in the back assistant message that
// duplicates a call already terminal earlier in the same agent turn. Returns
// the number deduped (for tests). Must run BEFORE kick_pending_tools promotes
// anything to Running.
std::size_t dedup_releaked_salvage_calls(Model& m) {
    if (m.d.current.messages.empty()) return 0;
    auto& msgs = m.d.current.messages;
    if (msgs.back().role != Role::Assistant) return 0;

    // Walk back to the first Assistant message of this turn (stop at the
    // User message that opened it). The turn is [turn_start, size).
    std::size_t turn_start = msgs.size();
    while (turn_start > 0 && msgs[turn_start - 1].role == Role::Assistant)
        --turn_start;

    // Collect (name, args.dump()) of every terminal call across the turn —
    // the set of side effects that have ALREADY happened (or failed) this
    // turn. args.dump() is canonical (key order is nlohmann's stable insert
    // order from the same parse path), so byte-equality == semantic equality
    // for the leaked-then-reparsed JSON.
    auto sig = [](const ToolUse& tc) {
        return tc.name.value + '\0' + tc.args.dump();
    };
    std::unordered_set<std::string> terminal_sigs;
    std::size_t terminal_salvaged = 0;   // how many salvaged calls already ran
    for (std::size_t i = turn_start; i < msgs.size(); ++i)
        for (const auto& tc : msgs[i].tool_calls)
            if (tc.is_terminal()) {
                terminal_sigs.insert(sig(tc));
                if (is_salvaged_call(tc.id)) ++terminal_salvaged;
            }

    // Runaway-loop guard. Exact-match dedup (below) only catches a re-leak
    // whose args are BYTE-identical to a prior terminal call. Weak local
    // models (qwen2.5-coder:7b via Ollama) routinely re-leak the SAME tool
    // with slightly drifted args every post-tool sub-turn — different scope,
    // reworded text — so dedup misses it and the agent loops forever (the
    // "stuck RUNNING" card the user sees). Once a turn has already executed
    // kMaxSalvagedPerTurn salvaged calls, treat any further pending salvaged
    // call as a leak loop and fail it WITHOUT running, regardless of args.
    // Structured tool calls (deliberate model intent) are never counted or
    // capped here — only synthetic salvaged leaks.
    //
    // 3 matches the industry consensus for the "doom-loop" / repeat threshold
    // on weak local models (Hermes-agent doom-loop=3; deer-flow warns at 3,
    // hard-stops at 5; LangChain's 15 is for strong hosted models). A leaked
    // tool call only ever comes from a small Ollama model that ignored the
    // local-model guidance, so a tight bound turns a multi-second "stuck"
    // hang into bounded degradation (~3 sub-turns) before we force the model
    // to answer in plain text.
    constexpr std::size_t kMaxSalvagedPerTurn = 3;

    // Memory-management tools (remember / forget / wipe_memory) are META: they
    // mutate the agent's own learned-memory store and must only fire on an
    // EXPLICIT user request ("remember X", "forget Y"). A weak local model
    // leaks them into `content` on greetings/small-talk ("remember: Hi there!",
    // "forget: Hi there!") — always junk, never the user's intent. A SALVAGED
    // call to one of these is therefore always illegitimate; fail it WITHOUT
    // running, regardless of budget or exact-match. Structured calls are left
    // alone (deliberate intent / the /slash activation path). This is the
    // single most-leaked tool class on small Ollama models, so it gets its own
    // hard block ahead of the generic loop guard.
    static constexpr std::string_view kMemoryTools[] = {
        "remember", "forget", "wipe_memory"};
    auto is_memory_tool = [](std::string_view n) {
        for (auto t : kMemoryTools) if (t == n) return true;
        return false;
    };
    std::size_t mem_blocked = 0;
    const auto now0 = std::chrono::steady_clock::now();
    for (auto& tc : msgs.back().tool_calls) {
        if (!tc.is_pending() && !tc.is_approved()) continue;
        if (!is_salvaged_call(tc.id)) continue;
        if (!is_memory_tool(tc.name.value)) continue;
        tc.status = ToolUse::Failed{
            tc.started_at(), now0,
            std::string{"not run ("} + tc.name.value
            + ": memory tools run only on an explicit user request)"};
        ++mem_blocked;
    }

    if (terminal_sigs.empty() && terminal_salvaged < kMaxSalvagedPerTurn)
        return mem_blocked;

    std::size_t deduped = 0;
    const auto now = std::chrono::steady_clock::now();
    for (auto& tc : msgs.back().tool_calls) {
        if (!tc.is_pending() && !tc.is_approved()) continue;
        if (!is_salvaged_call(tc.id)) continue;   // only dedup salvaged leaks
        const bool is_exact_releak = terminal_sigs.contains(sig(tc));
        const bool over_budget     = terminal_salvaged >= kMaxSalvagedPerTurn;
        if (!is_exact_releak && !over_budget) continue;
        // Re-leak of a call already settled this turn (exact match), OR the
        // turn has blown its salvage budget (drifting-args leak loop).
        // Resolve as Failed WITHOUT running it — any real side effect already
        // happened. The message tells the model to stop re-emitting and finish.
        tc.status = ToolUse::Failed{
            tc.started_at(), now,
            over_budget && !is_exact_releak
                ? std::string{"not run (too many repeated tool calls this "
                  "turn)"}
                : std::string{"not run (duplicate — this exact call already "
                  "ran this turn; its result is above)"}};
        ++deduped;
    }
    return deduped + mem_blocked;
}

namespace {

// Body of the synthetic summarisation prompt. Identical text to what
// CompactContext used to push directly into `messages` — we hoist it
// here so the compaction wire payload can build it without polluting
// the transcript. Mirrors Claude Code's `mm8` summary prompt (binary
// near offset 134600). The schema (Task / State / Discoveries / Next
// Steps / Context-to-Preserve) is deliberately verbose: it nudges the
// model to write a recoverable summary instead of a one-paragraph
// précis that loses operationally-load-bearing details.
constexpr std::string_view kCompactionSummaryPrompt =
    "You have been working on the task described above but have "
    "not yet completed it. Write a continuation summary that "
    "will allow you (or another instance of yourself) to resume "
    "work efficiently in a future context window where the "
    "conversation history will be replaced with this summary. "
    "Your summary should be structured, concise, and actionable. "
    "Include:\n"
    "1. Task Overview\n"
    "  The user's core request and success criteria\n"
    "  Any clarifications or constraints they specified\n"
    "2. Current State\n"
    "  What has been completed so far\n"
    "  Files created, modified, or analyzed (with paths if relevant)\n"
    "  Key outputs or artifacts produced\n"
    "3. Important Discoveries\n"
    "  Technical constraints or requirements uncovered\n"
    "  Decisions made and their rationale\n"
    "  Errors encountered and how they were resolved\n"
    "  What approaches were tried that didn't work (and why)\n"
    "4. Next Steps\n"
    "  Specific actions needed to complete the task\n"
    "  Any blockers or open questions to resolve\n"
    "  Priority order if multiple steps remain\n"
    "5. Context to Preserve\n"
    "  User preferences or style requirements\n"
    "  Domain-specific details that aren't obvious\n"
    "  Any promises made to the user\n"
    "Be concise but complete \xe2\x80\x94 err on the side of "
    "including information that would prevent duplicate work or "
    "repeated mistakes. Write in a way that enables immediate "
    "resumption of the task. Do not call any tools; just write "
    "the summary text. Wrap the summary in <summary></summary> "
    "tags.";

// Body prefix wrapping the model's summary text into a synthetic User
// message that goes on the wire in place of [0, up_to_index). The
// trailing "Continue…" directive (CC trick, binary near offset
// 77409806) suppresses the model's tendency to start with a "let me
// recap what we were doing" preamble.
std::string wrap_summary_as_user_text(const std::string& summary) {
    return "This session is being continued from a previous "
           "conversation that ran out of context. The summary "
           "below covers the earlier portion of the "
           "conversation; recent messages are preserved "
           "verbatim after this summary.\n\nSummary:\n"
         + summary
         + "\n\nContinue the work from where it left off "
           "without re-acknowledging this summary or recapping "
           "what was happening. Pick up the last task as if "
           "the break never happened.";
}

// Bytes-based prefix token estimate over an arbitrary wire view
// (NOT a Thread). Used internally by the soft-trim path; matches the
// approximation that public estimate_wire_tokens / estimate_prefix_tokens
// use so all three agree on "is this payload too big."
int estimate_messages_tokens(const std::vector<Message>& v) {
    constexpr double kBytesPerToken  = 3.5;
    constexpr int    kTokensPerImage = 1500;
    std::size_t bytes = 0;
    int images = 0;
    for (const auto& m : v) {
        bytes += m.text.size();
        bytes += m.streaming_text.size();
        bytes += m.pending_stream.size();
        images += static_cast<int>(m.images.size());
        for (const auto& tc : m.tool_calls) {
            bytes += tc.name.value.size();
            bytes += tc.args_streaming.size();
            bytes += tc.output().size();
            bytes += tc.progress_text().size();
        }
    }
    return static_cast<int>(static_cast<double>(bytes) / kBytesPerToken)
         + images * kTokensPerImage;
}

// Adaptive soft-trim: drop oldest entries from a wire view until its
// estimated token cost fits a ceiling. Preserves the leading entry
// (typically the compaction-summary synth user OR the first real
// user turn) so the model retains task-rooting context, and ensures
// the result still starts with a User per Anthropic's wire rule.
//
// This is the workflow-protection knob. When the model is mid-tool-
// burst and the transcript has grown past context_max, we DON'T yank
// the agent into a compaction round (which would surface as "your
// agent got stopped"). We just trim the oldest raw turns off the
// wire payload and let the loop continue. Compaction is then a
// strictly-better optimisation the user can run at their leisure;
// the agent never observes a wedge.
//
// The trim is REVERSIBLE: nothing about the transcript or any
// CompactionRecord is mutated. Two requests apart with different
// soft-trim ceilings produce different wire payloads from the same
// source state. The next user turn that submits with more context
// available will send a fatter prefix automatically.
void soft_trim_to_ceiling(std::vector<Message>& v, int ceiling) {
    if (ceiling <= 0 || v.size() <= 1) return;
    // Sentinel: the leading entry stays put. Trim from index 1 forward
    // until we fit. If we run out of trimmable entries we send what
    // we have — the upstream will hard-reject with a clear error,
    // which is strictly better than the in-tool-burst yank.
    while (estimate_messages_tokens(v) > ceiling && v.size() > 1) {
        v.erase(v.begin() + 1);
    }
    // Drop any leading Assistants exposed by the trim — the wire
    // must start with a User. (If [0] was a compaction-summary user,
    // it remains; if it was a real first-user-turn and we never
    // touched [0], we're already fine. The pathological case is
    // when [0] gets dropped by a future change — defensive guard.)
    while (!v.empty() && v.front().role == Role::Assistant) {
        v.erase(v.begin());
    }
}


// Normal-turn wire payload: if the thread has compaction records,
// replace the prefix [0, latest.up_to_index) with one synthetic User
// message carrying the summary. Tail [latest.up_to_index..end) goes
// verbatim, preserving the user's recent work as anchor for the model.
// No compactions → the transcript ships as-is.
//
// Anthropic wire requirement: the messages array must start with a
// User. The synthetic summary IS a User, and if it's absent the first
// real message at index 0 has historically been a User too (the user's
// initial prompt), so the invariant holds either way.
std::vector<Message> wire_messages_for_impl(const Thread& t) {
    if (t.compactions.empty()) return t.messages;
    const auto& rec = t.compactions.back();
    if (rec.up_to_index == 0 || rec.up_to_index > t.messages.size()) {
        return t.messages;  // defensive: malformed record → send raw transcript
    }
    std::vector<Message> out;
    out.reserve(1 + (t.messages.size() - rec.up_to_index));
    Message summary_msg;
    summary_msg.role = Role::User;
    summary_msg.is_compact_summary = true;   // tag for any downstream code that still keys on it
    summary_msg.text = wrap_summary_as_user_text(rec.summary);
    out.push_back(std::move(summary_msg));
    for (std::size_t i = rec.up_to_index; i < t.messages.size(); ++i) {
        out.push_back(t.messages[i]);
    }
    return out;
}

// Compaction-kickoff wire payload: send the prefix the user wants to
// summarise (with any prior compactions already applied so we don't
// re-send already-summarised turns raw), followed by the summarisation
// prompt as a synthetic User turn. The model's reply IS the summary;
// it streams into `m.s.compaction_buffer` rather than into `messages`.
//
// `context_max` is consulted to keep the request itself from blowing
// the context window — we trim the OLDEST raw turns of the prefix
// (preserving the leading User-wire-shape, never trimming a synthetic
// summary already at the head) until the estimate fits ~65% of the
// window, leaving headroom for the prompt + the summary response.
std::vector<Message> wire_messages_for_compaction(const Thread& t, int context_max) {
    // Start from the already-compaction-substituted view so stacked
    // compactions don't double-count the summarised prefix.
    std::vector<Message> base = wire_messages_for_impl(t);

    // Trim from the front until we fit ~65% of context_max. Token
    // estimate uses the same approximation as estimate_prefix_tokens
    // (bytes / 3.5 + images), inlined here against an arbitrary
    // vector instead of a Thread.
    auto estimate = [](const std::vector<Message>& v) -> int {
        constexpr double kBytesPerToken  = 3.5;
        constexpr int    kTokensPerImage = 1500;
        std::size_t bytes = 0;
        int images = 0;
        for (const auto& m : v) {
            bytes += m.text.size();
            bytes += m.streaming_text.size();
            bytes += m.pending_stream.size();
            images += static_cast<int>(m.images.size());
            for (const auto& tc : m.tool_calls) {
                bytes += tc.name.value.size();
                bytes += tc.args_streaming.size();
                bytes += tc.output().size();
                bytes += tc.progress_text().size();
            }
        }
        return static_cast<int>(static_cast<double>(bytes) / kBytesPerToken)
             + images * kTokensPerImage;
    };

    if (context_max > 0) {
        const int ceiling = static_cast<int>(static_cast<double>(context_max) * 0.65);
        while (estimate(base) > ceiling && base.size() > 1) {
            base.erase(base.begin());
        }
        // Drop any leading Assistants exposed by the trim — Anthropic
        // requires the wire to start with a User.
        while (!base.empty() && base.front().role == Role::Assistant) {
            base.erase(base.begin());
        }
    }

    // Append the synthetic summarisation prompt as the trailing User.
    Message synth;
    synth.role = Role::User;
    synth.text = std::string{kCompactionSummaryPrompt};
    base.push_back(std::move(synth));
    return base;
}

} // namespace

std::vector<Message> wire_messages_for(const Thread& t) {
    return wire_messages_for_impl(t);
}

int estimate_wire_tokens(const Thread& t) {
    // Same bytes/3.5 + ~1500-per-image approximation as
    // estimate_prefix_tokens(Thread), but against the wire view so
    // any compaction summary substitution is accounted for. Auto-
    // compaction triggers MUST use this; using the raw-transcript
    // estimate would re-fire compaction immediately after every
    // round because the user-visible transcript never shrinks.
    constexpr double kBytesPerToken  = 3.5;
    constexpr int    kTokensPerImage = 1500;
    auto wire = wire_messages_for_impl(t);
    std::size_t bytes = 0;
    int images = 0;
    for (const auto& m : wire) {
        bytes += m.text.size();
        bytes += m.streaming_text.size();
        bytes += m.pending_stream.size();
        images += static_cast<int>(m.images.size());
        for (const auto& tc : m.tool_calls) {
            bytes += tc.name.value.size();
            bytes += tc.args_streaming.size();
            bytes += tc.output().size();
            bytes += tc.progress_text().size();
        }
    }
    return static_cast<int>(static_cast<double>(bytes) / kBytesPerToken)
         + images * kTokensPerImage;
}

Cmd<Msg> launch_stream(Model& m) {
    // Defer the wire-payload build to the worker. The UI thread used
    // to spend ~5-50 ms here on long threads (full t.messages deep
    // copy via wire_messages_for_impl, plus soft_trim_to_ceiling's
    // estimate loop) between the user pressing Enter and the next
    // render — visible as input lag on the "my message appears" frame.
    // Now the UI returns to render after a few microseconds of cheap
    // capture work; the heavy lift runs concurrently with the first
    // post-submit paint.
    //
    // What stays on the UI thread (must — they read the live Model):
    //   • cancel token mint + stash onto active_ctx
    //   • shallow Thread copy into the task closure (vector of
    //     Messages; each Message itself contains owned strings/vectors
    //     so the copy is deep — but it's the same copy wire_messages
    //     _for_impl was doing anyway, just relocated)
    //
    // What moves to the worker:
    //   • compaction-vs-normal payload branch
    //   • soft_trim_to_ceiling
    //   • tools::registry() walk
    //   • provider::anthropic::default_system_prompt()
    //   • deps().stream(...) (already async)
    //
    // Mint a fresh cancel token per turn and stash it on the active
    // ctx so the Esc handler (Msg::CancelStream) can flip it. The
    // worker holds its own shared_ptr via the captured request; storing
    // it inside the phase variant on the UI thread is safe — both
    // sides only ever load/store the atomic flag. Caller has already
    // transitioned us into an active phase by the time launch_stream
    // runs, so active_ctx is non-null here.
    auto cancel = std::make_shared<http::CancelToken>();
    if (auto* a = active_ctx(m.s.phase)) a->cancel = cancel;

    // Snapshot the per-turn retry counter so the worker can stamp it on
    // the wire as x-stainless-retry-count. Reflecting the real attempt
    // number (instead of a hard-coded 0) is what the Anthropic SDK / Zed
    // do; the edge uses it for routing and to avoid penalising retried
    // traffic.
    const int retry_count = [&] {
        if (const auto* a = active_ctx(m.s.phase)) return a->transient_retries;
        return 0;
    }();

    // Capture the snapshot the worker needs. The Thread copy is the
    // one unavoidable cost; everything else is small.
    Thread thread_snapshot = m.d.current;
    const bool compacting  = m.s.compacting;
    const int  context_max = m.s.context_max;
    std::string model_id   = m.d.model_id.value;
    auth::AuthHeader auth  = deps().auth;

    // Look up the selected model's supports_tools from available_models.
    // Ollama models have this set via /api/show probe at list time. If
    // the model reports supports_tools=false, we skip advertising tools
    // entirely (Zed-style: the model can only be used for plain chat).
    // std::nullopt = unknown/not probed = fall through to heuristic.
    std::optional<bool> model_supports_tools;
    int model_context_window = 0;
    for (const auto& mi : m.d.available_models) {
        if (mi.id.value == model_id) {
            model_supports_tools = mi.supports_tools;
            model_context_window = mi.context_window;
            break;
        }
    }

    return Cmd<Msg>::task(
        [thread = std::move(thread_snapshot),
         compacting, context_max, retry_count,
         model_id = std::move(model_id),
         model_supports_tools,
         model_context_window,
         auth = std::move(auth),
         cancel]
        (std::function<void(Msg)> dispatch) mutable {
        // Build wire payload off the UI thread.
        provider::Request req;
        req.model         = std::move(model_id);
        // Per-model output-token ceiling. The default (kSafeMaxTokens=16384)
        // is shared across reasoning + tool JSON for the whole turn; a large
        // `edit` (verbatim old_text + new_text) can overrun it and arrive
        // truncated as "arguments look incomplete". Raise it to the model's
        // real output capacity. See max_output_tokens_for in catalog.hpp.
        req.max_tokens    = max_output_tokens_for(req.model);
        // Model's real context window (probed from Ollama /api/show; 0 for
        // hosted models). The Ollama transport turns this into options.num_ctx
        // so long agent conversations aren't truncated to Ollama's tiny default.
        req.context_window = model_context_window;
        // System prompt is chosen PER PROVIDER. Anthropic (Claude) gets the
        // full Claude agentic prompt. Ollama (native /api/chat) gets its own
        // local-tuned prompt. Other OpenAI-compatible backends get the
        // openai local-model prompt. The verbose Claude prose primes small
        // local models to over-call tools and some break outright on it.
        const auto& sel_now = provider::active();
        const bool openai_provider = sel_now.kind == provider::Kind::OpenAI;
        if (openai_provider && sel_now.openai_endpoint.native_api)
            req.system_prompt = provider::ollama::system_prompt();
        else if (openai_provider)
            req.system_prompt = provider::openai::local_model_system_prompt();
        else
            req.system_prompt = provider::anthropic::default_system_prompt();
        // Weak models (small local / coder ids) still hide a few footgun
        // tools below; the prompt no longer branches on it.
        const bool weak_model = is_weak_model(req.model);
        // First-class weak-model support (agent-zero style): on the Ollama
        // native endpoint, weak models get the JSON-protocol path — no native
        // `tools` array, the tool catalog is inlined in the prompt and the
        // model answers with one {tool_name,tool_args} object. Tiny models
        // follow "emit one JSON object" far more reliably than the native
        // function-call channel. Capable/large local models keep the native
        // structured channel.
        req.json_protocol =
            weak_model && openai_provider && sel_now.openai_endpoint.native_api;
        req.cancel        = cancel;
        req.auth          = std::move(auth);
        req.retry_count   = retry_count;

        // Ollama capability gate: if /api/show reported the model does NOT
        // support tools (supports_tools == false), skip advertising ANY
        // tools — the model can only be used for plain chat. This matches
        // Zed's behavior: models without the "tools" capability don't get
        // tools on the wire, so they can't leak phantom calls or loop.
        // std::nullopt means unknown / not probed (non-Ollama endpoints or
        // probe failure): fall through to the normal heuristic.
        const bool tools_disabled_by_capability =
            model_supports_tools.has_value() && !model_supports_tools.value();

        // Wire payload diverges on compaction kickoff:
        //   normal turn  → wire_messages_for(thread) substitutes any
        //                  prior compaction summary in place of its
        //                  covered prefix.
        //   compaction   → wire_messages_for_compaction(thread, ctx_max)
        //                  appends the synthetic summarisation prompt and
        //                  trims the raw prefix to fit context_max. Tools
        //                  are omitted from the wire so the model can only
        //                  reply with text — a tool_use during compaction
        //                  would have nowhere to land (we don't surface
        //                  the synthetic turn in the UI).
        if (compacting) {
            req.messages = wire_messages_for_compaction(thread, context_max);
            // req.tools left empty — summarisation is text-only.
        } else {
            req.messages = wire_messages_for_impl(thread);
            // Adaptive soft-trim: if the wire view is still over the
            // window (no compactions yet, or the latest one is stale
            // and the user has run many turns since), drop oldest raw
            // turns from the wire until it fits ~95% of context_max.
            if (context_max > 0) {
                const int soft_ceiling = static_cast<int>(
                    static_cast<double>(context_max) * 0.95);
                soft_trim_to_ceiling(req.messages, soft_ceiling);
            }
            // All tools, every profile. Gating is the policy layer's job
            // (`tool::DynamicDispatch::needs_permission`, called from
            // `kick_pending_tools`). EXCEPTION 1: if Ollama's /api/show
            // reported supports_tools=false, skip ALL tools — the model
            // can only be used for plain chat. EXCEPTION 2: weak local
            // models hallucinate `skill` and the memory tools on greetings
            // /small talk — don't put those on the wire for weak models.
            // (Memory tools still run via /slash command; skills /skill-name.)
            if (!tools_disabled_by_capability) {
                auto weak_hidden = [](std::string_view n) {
                    return n == "skill" || n == "remember"
                        || n == "forget" || n == "wipe_memory";
                };
                for (const auto& t : tools::wire_tools()) {
                    if (weak_model && weak_hidden(t.name.value)) continue;
                    req.tools.push_back({t.name.value, t.description, t.input_schema,
                                         t.eager_input_streaming});
                }
            }
        }

        // Suppress every event after the token is tripped — including the
        // worker's own terminal StreamError("cancelled"). The UI thread
        // already did the cancel-side cleanup synchronously in the
        // CancelStream handler (phase=Idle, status="cancelled", tool
        // calls marked Cancelled, empty placeholder dropped), so a
        // trailing event here would either be a no-op against an Idle
        // phase or — worse — a write into a brand-new turn's state.
        // The check matches the worker's own cancellation gate, so once
        // the token is tripped no further events flow into the reducer
        // from this worker.
        auto guarded = [dispatch, cancel](Msg m) {
            if (cancel && cancel->is_cancelled()) return;
            dispatch(std::move(m));
        };
        try {
            deps().stream(std::move(req), [guarded](Msg m) {
                guarded(std::move(m));
            });
        } catch (const std::exception& e) {
            // The stream backend threw before producing a terminal event —
            // surface it as StreamError so the UI doesn't hang on the spinner.
            guarded(StreamError{std::string{"stream backend: "} + e.what()});
        } catch (...) {
            guarded(StreamError{"stream backend: unknown exception"});
        }
    });
}

Cmd<Msg> run_tool(ToolCallId id, ToolName tool_name, nlohmann::json args) {
    // task_isolated, NOT task: a tool that wedges (e.g. read on a hung NFS
    // mount, bash on a process that won't unblock) must not consume a slot
    // in the shared BG worker pool. With Cmd::task the pool's max workers
    // — std::max(4, hw_concurrency) — get permanently filled by zombie
    // threads stuck in syscalls, and subsequent tools queue forever even
    // though the UI watchdog has long since flipped them to Failed. This
    // surfaced as "tools randomly get stuck" once enough wedged calls
    // accumulated. Per-call detached thread costs ~100-300 µs of
    // construction; tools run seconds apart so it's noise.
    return Cmd<Msg>::task_isolated(
        [id = std::move(id),
         name = std::move(tool_name),
         args = std::move(args)]
        (std::function<void(Msg)> dispatch) {
            // Install a thread-local progress sink *before* dispatch so the
            // subprocess runner inside the tool can stream stdout+stderr to
            // the UI as bytes arrive. RAII scope guarantees the sink is
            // cleared even if the tool throws, so the next tool run can't
            // inherit a stale dispatch lambda.
            agentty::tools::progress::Scope progress_scope{
                [dispatch, id](std::string_view snapshot) {
                    dispatch(ToolExecProgress{id, std::string{snapshot}});
                }};
            try {
                auto result = tool::DynamicDispatch::execute(name.value, args);
                if (result) {
                    dispatch(ToolExecOutput{id, std::move(result->text)});
                } else {
                    dispatch(ToolExecOutput{id,
                        std::unexpected(std::move(result).error())});
                }
            } catch (const std::exception& e) {
                // DynamicDispatch already catches tool exceptions, but guard
                // against anything in the dispatch infrastructure itself so
                // the tool never gets stuck in Running with no terminal Msg.
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown(
                        std::string{"dispatch error: "} + e.what()))});
            } catch (...) {
                dispatch(ToolExecOutput{id, std::unexpected(
                    tools::ToolError::unknown("dispatch error: unknown exception"))});
            }
        });
}

namespace {

// ── Path-aware parallel scheduling ───────────────────────────────────────────
//
// The effect-only gate (is_parallel_safe) is coarse: ANY writer forces
// exclusive access, so two edits to DIFFERENT files serialise, and a read of
// a.c blocks behind an unrelated write to b.c. That's correct but leaves
// latency on the table — the model routinely emits a fan of edits across
// disjoint files in one turn.
//
// We refine it the same way mcp-cpp's cap::scheduler does: track the SET OF
// PATHS each running/promoted tool touches, and let a writer (or reader)
// proceed concurrently when its paths don't OVERLAP any active path. A tool
// whose target path can't be extracted, or any Exec (bash — unbounded blast
// radius), keeps the conservative exclusive behaviour.

// Pull the fs target(s) out of a tool call's args, mirroring the built-in
// tool vocabulary (read/write/edit/find_definition use path|file_path;
// grep/glob/list_dir scope under an optional dir). Empty ⇒ unknown target.
[[nodiscard]] std::vector<std::string> tc_paths(const ToolUse& tc) {
    std::vector<std::string> out;
    if (!tc.args.is_object()) return out;
    auto take = [&](const char* k) {
        if (auto it = tc.args.find(k); it != tc.args.end() && it->is_string()) {
            auto s = it->get<std::string>();
            if (!s.empty()) out.push_back(std::move(s));
        }
    };
    take("path"); take("file_path"); take("filepath"); take("filename");
    const auto& n = tc.name.value;
    if (n == "grep" || n == "glob" || n == "list_dir") {
        take("dir"); take("directory"); take("root");
    }
    return out;
}

// Prefix-aware path overlap: identical, or one is a directory ancestor of the
// other (with a separator at the boundary so "src" doesn't match "srcfoo").
[[nodiscard]] bool paths_overlap(std::string_view a, std::string_view b) {
    if (a == b) return true;
    auto under = [](std::string_view shorter, std::string_view longer) {
        return longer.size() > shorter.size()
            && longer.substr(0, shorter.size()) == shorter
            && (shorter.back() == '/' || longer[shorter.size()] == '/');
    };
    return a.size() < b.size() ? under(a, b) : under(b, a);
}

} // namespace

SchedDecision schedule_parallel_batch(const std::vector<ToolUse>& batch) {
    auto effects_of = [](const ToolName& n) -> tools::EffectSet {
        if (const auto* sp = tools::spec::lookup(n.value)) return sp->effects;
        return tools::EffectSet{{tools::Effect::Exec}};
    };
    tools::EffectSet active_effects;
    std::vector<std::string> active_paths;
    bool active_unbounded = false;
    auto note = [&](const ToolUse& tc, tools::EffectSet fx) {
        active_effects |= fx;
        auto ps = tc_paths(tc);
        if (fx.has(tools::Effect::Exec)
            || (fx.has(tools::Effect::WriteFs) && ps.empty()))
            active_unbounded = true;
        for (auto& p : ps) active_paths.push_back(std::move(p));
    };
    auto can_run = [&](const ToolUse& tc, tools::EffectSet want) -> bool {
        if (tools::is_parallel_safe(active_effects, want)) return true;
        if (want.has(tools::Effect::Exec) || active_unbounded) return false;
        auto ps = tc_paths(tc);
        if (ps.empty()) return false;
        for (const auto& cp : ps)
            for (const auto& ap : active_paths)
                if (paths_overlap(cp, ap)) return false;
        return true;
    };
    // Seed with whatever's already running.
    for (const auto& tc : batch)
        if (tc.is_running()) note(tc, effects_of(tc.name));
    // Greedily promote pending/approved calls in submission order.
    SchedDecision out;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        const auto& tc = batch[i];
        if (!tc.is_pending() && !tc.is_approved()) continue;
        const auto want = effects_of(tc.name);
        if (!can_run(tc, want)) continue;   // stays pending; advances next kick
        note(tc, want);
        out.promote.push_back(i);
    }
    return out;
}

// ── Doom-loop circuit breaker ────────────────────────────────────────────────
// Caps a non-converging weak-model tool loop. See the header doc on
// agent_loop_should_break for the rationale. Walks BACK from the end of the
// transcript over the current agent run (everything after the last real User
// message that isn't a synthetic TOOL-RESULT carrier) and applies two caps.
std::optional<LoopBreak> agent_loop_should_break(
        const std::vector<Message>& messages) {
    // Tunables. Generous enough that a legitimate multi-step task (search →
    // read → edit → verify …) never trips, tight enough that a stuck model
    // bails in seconds rather than spinning until the user hits Esc.
    constexpr int kRepeatLimit  = 3;   // same failing call N times → stop
    constexpr int kMaxToolTurns = 25;  // tool-call turns w/o a text answer

    // Find the start of the current run: the last User message. (Sub-turn
    // continuations push Assistant placeholders; tool results live inside the
    // Assistant messages' tool_calls, so a User message only appears at a real
    // human turn boundary.)
    std::size_t run_start = 0;
    for (std::size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == Role::User) { run_start = i + 1; break; }
    }

    int tool_turns = 0;
    // (name + canonical args) → [count, all_failed]. Detects a repeated dead
    // call regardless of the synthetic id minted per attempt.
    std::unordered_map<std::string, std::pair<int, bool>> seen;
    for (std::size_t i = run_start; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (msg.role != Role::Assistant || msg.tool_calls.empty()) continue;
        ++tool_turns;
        for (const auto& tc : msg.tool_calls) {
            // Only settled calls inform the breaker; in-flight ones are the
            // batch we're about to (re)kick.
            if (!tc.is_terminal()) continue;
            std::string key = tc.name.value + '\0'
                + (tc.args.is_null() ? std::string{} : tc.args.dump());
            auto& [count, all_failed] = seen[key];
            if (count == 0) all_failed = true;
            ++count;
            if (!tc.is_failed()) all_failed = false;
        }
    }

    for (const auto& [key, v] : seen) {
        const auto& [count, all_failed] = v;
        if (count >= kRepeatLimit && all_failed) {
            auto nul = key.find('\0');
            std::string name = nul == std::string::npos ? key : key.substr(0, nul);
            return LoopBreak{
                "Stopped: the `" + name + "` tool was called "
                + std::to_string(count) + " times with the same arguments and "
                "failed every time. Re-trying the identical call won't help — "
                "change the arguments (check the path/target exists, or pick a "
                "different tool), or answer the user directly with what you "
                "already know."};
        }
    }
    if (tool_turns >= kMaxToolTurns) {
        return LoopBreak{
            "Stopped after " + std::to_string(tool_turns) + " tool steps "
            "without finishing. Summarise what you found and answer the user "
            "directly, or ask them how to proceed."};
    }
    return std::nullopt;
}

Cmd<Msg> kick_pending_tools(Model& m) {
    if (m.d.current.messages.empty()) return Cmd<Msg>::none();
    auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return Cmd<Msg>::none();

    // Bail early if the session is already Idle. This is the
    // late-arrival window: a tool worker thread can return a
    // ToolExecOutput AFTER the user has cancelled (Esc → phase=Idle)
    // or after StreamError dropped to Idle. In those cases there's
    // no in-flight request to attach a sub-turn to, no ctx to take,
    // and the result has nowhere to go. Pre-guard means we don't
    // tumble into the take_active_ctx(...).value() sites below with
    // an empty optional source and abort the process with
    // bad_optional_access.
    //
    // Tools that were Pending/Approved at cancel-time are already
    // marked Failed/Rejected by CancelStream's teardown loop, so
    // there's nothing left to advance anyway.
    if (m.s.is_idle()) return Cmd<Msg>::none();

    // Drop re-leaked salvaged calls (weak local models re-emitting a tool
    // call they already ran this turn) BEFORE any promotion to Running, so a
    // duplicate never executes a second side effect. See
    // dedup_releaked_salvage_calls.
    dedup_releaked_salvage_calls(m);

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;

    // Effect- and path-aware scheduler. When the assistant emits multiple
    // tool calls in one turn they all start Pending; maya's BG worker pool
    // runs Task cmds on independent threads, so any set of tools we dispatch
    // in this tick runs concurrently. The scheduling decision — which
    // pending/approved calls may join the in-flight wave — is computed by
    // the PURE planner `schedule_parallel_batch` (defined just above, unit-
    // tested in scheduler_path_test). Keeping the live gate as a thin
    // consumer of that planner means the test exercises the real rule, not a
    // parallel reimplementation that could silently drift.
    //
    // Deferred (conflicted) tools stay Pending. When the blocking tool's
    // ToolExecOutput lands, update.cpp re-fires kick_pending_tools and the
    // now-unblocked siblings advance.
    const auto plan = schedule_parallel_batch(last.tool_calls);
    std::vector<bool> promote(last.tool_calls.size(), false);
    for (auto i : plan.promote) promote[i] = true;

    for (std::size_t i = 0; i < last.tool_calls.size(); ++i) {
        auto& tc = last.tool_calls[i];
        // Approved: user already granted permission; advance it exactly
        // like a Pending-but-no-permission-needed tool, minus the
        // permission check. Keeps the planner as the single source of
        // truth for scheduling.
        const bool ready = tc.is_pending() || tc.is_approved();
        if (ready) {
            const bool needs_perm = tc.is_approved()
                ? false
                : (!m.d.session_grants.contains(tc.name.value)
                   && tool::DynamicDispatch::needs_permission(tc.name.value, m.d.profile));
            if (needs_perm && !m.d.pending_permission) {
                m.d.pending_permission = PendingPermission{
                    tc.id, tc.name,
                    "Tool " + tc.name.value + " needs permission under "
                        + std::string{ui::profile_label(m.d.profile)} + " profile"};
                // Streaming → AwaitingPermission. The active ctx
                // (cancel token, retry state, started stamp) flows
                // through unchanged; the phase tag is the only thing
                // that moves.
                auto ctx = take_active_ctx(std::move(m.s.phase));
                m.s.phase = phase::AwaitingPermission{std::move(ctx).value()};
                return Cmd<Msg>::none();
            }
            if (!needs_perm) {
                // Effect/path-compatibility gate: the planner decided this
                // tool conflicts with the in-flight wave (or an earlier
                // sibling promoted this same tick). Leave it Pending;
                // kick_pending_tools re-fires on every terminal
                // ToolExecOutput, so deferred siblings advance
                // automatically without explicit requeueing.
                if (!promote[i]) {
                    any_pending = true;
                    continue;
                }

                // started_at was stamped at StreamToolUseStart so the
                // timer covers the full card lifetime (args streaming +
                // execution). Preserve it as we move into Running.
                tc.status = ToolUse::Running{tc.started_at(), {}};
                cmds.push_back(run_tool(tc.id, tc.name, tc.args));

                // Tool wall-clock watchdog removed at user request.
                // Tools now run for as long as their worker takes;
                // user cancels via Esc.  bash / diagnostics still
                // self-timeout via subprocess.cpp.  Tools that wedge
                // in a blocking syscall (slow NFS, dead FUSE mount)
                // will leave the card in Running until cancellation —
                // accept this trade-off explicitly.

                // {Streaming, AwaitingPermission, ExecutingTool} →
                // ExecutingTool. Source is whatever phase produced
                // the now-running tool: Streaming for a tool the
                // model just emitted, AwaitingPermission for one the
                // user just granted, ExecutingTool for a sibling
                // promotion within the same batch. Active ctx flows
                // through unchanged. ExecutingTool is non-Idle so
                // active() flips on automatically: the Tick subscrip-
                // tion stays armed, the spinner advances, the view's
                // live elapsed timer keeps ticking — without that
                // the UI looks frozen on long-running bash commands.
                auto ctx = take_active_ctx(std::move(m.s.phase));
                m.s.phase = phase::ExecutingTool{std::move(ctx).value()};
                any_pending = true;
            }
        } else if (tc.is_running()) {
            any_pending = true;
        }
    }

    if (!any_pending) {
        const bool has_results = std::ranges::any_of(last.tool_calls, [](const auto& tc){
            return tc.is_terminal();
        });
        if (has_results) {
            // ── Doom-loop circuit breaker ────────────────────────────────
            // Before spending another model completion, check whether this
            // agent run has stopped converging (a weak model re-trying a dead
            // call, or a runaway tool-step count). If so, DON'T re-stream:
            // surface the nudge as the run's final assistant turn and drop to
            // Idle, so the loop ends in seconds instead of spinning until the
            // user hits Esc. The nudge text also lands in history, so if the
            // user follows up the model sees why it stopped.
            if (auto brk = agent_loop_should_break(m.d.current.messages)) {
                m.s.phase = phase::Idle{};
                Message note;
                note.role = Role::Assistant;
                note.text = std::move(brk->reason);
                m.d.current.messages.push_back(std::move(note));
                deps().save_thread(m.d.current);
                return Cmd<Msg>::batch(std::move(cmds));
            }

            // when the prefix estimate crossed 90% of context_max,
            // surfacing as a user-visible "context limit reached —
            // compacting before continuing…" toast that yanked the
            // agent out of a multi-tool burst. That broke workflow
            // hard: the user's agent loop would just *stop* mid-task
            // while the model rewrote a summary.
            //
            // Now: launch_stream's soft-trim on the normal-turn path
            // drops oldest raw turns from the wire view until it fits
            // ~95% of context_max, transparently. The agent keeps
            // running. Compaction becomes a strictly-better optimisation
            // (summary > truncation) the user can invoke at their
            // leisure via /compact, or the auto-trigger picks up on
            // the NEXT user submit. Workflow uninterrupted.
            //
            // The trim is wire-only: the transcript stays whole, the
            // dropped turns reappear on subsequent requests if context
            // frees up (e.g. after a manual compact).

            // ExecutingTool → Streaming (post-tool sub-turn). Active
            // ctx flows through: cancel token's still alive (the
            // request is still open, sub-turn streams over the same
            // SSE), retry counters preserved.
            //
            // We push a fresh Assistant placeholder so the wire-layer
            // tool_use ↔ tool_result pairing stays correct (the
            // transport synthesises one User-with-tool_results turn
            // per Assistant Message; collapsing sub-turns into the
            // same Message would interleave new tool_use blocks
            // before their tool_results). The VIEW collapses these
            // sub-turn Messages back into one visual Turn via the
            // shared `ui::turn_run_end` helper in build_live_tail
            // (and freeze_range), matching agent_session's
            // "one agent turn = one Turn" shape.
            //
            // No freeze fires here: mid-stream freezing would split
            // an in-flight assistant run across the frozen / live
            // boundary, producing two visual Turns where the user
            // should see one. The single freeze site is in
            // `finalize_turn` once `phase::Idle` is reached.
            auto ctx = take_active_ctx(std::move(m.s.phase));
            // Re-arm the stall watchdog across the tool boundary. No SSE
            // events flow during ExecutingTool, so last_event_at is as
            // old as the last delta before the tool ran — minutes, for a
            // long `bash`/`gh run watch`. Carrying it into Streaming lets
            // a Tick land before the new sub-turn's StreamStarted resets
            // it, firing a spurious "stream stalled — no events for Ns".
            // The sub-turn is a fresh wire phase; start its clock now.
            auto now = std::chrono::steady_clock::now();
            ctx.value().last_event_at = now;
            ctx.value().retry         = retry::Fresh{};
            m.s.phase = phase::Streaming{std::move(ctx).value()};
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            cmds.push_back(launch_stream(m));
        } else {
            // ExecutingTool → Idle (no continuation). Active ctx is
            // discarded — the request is finished, the cancel token
            // can drop, the retry counters reset on the next user
            // turn. Idle drops active() to false, stopping the Tick
            // subscription.
            m.s.phase = phase::Idle{};
        }
    }
    return Cmd<Msg>::batch(std::move(cmds));
}

Cmd<Msg> fetch_models() {
    return Cmd<Msg>::task([](std::function<void(Msg)> dispatch) {
        try {
            std::vector<ModelInfo> models;
            const auto& sel = provider::active();
            if (sel.kind == provider::Kind::OpenAI) {
                models = provider::openai::list_models(deps().auth,
                                                       sel.openai_endpoint);
            } else {
                models = provider::anthropic::list_models(deps().auth);
            }
            dispatch(ModelsLoaded{std::move(models)});
        } catch (const std::exception& e) {
            // Dispatch an EMPTY ModelsLoaded (not StreamError) so the
            // reducer always clears `models_loading` and the model
            // picker drops out of "Loading models…" into a "no models"
            // state the user can escape. A StreamError here would leave
            // the picker spinning forever after a failed provider switch.
            // Still surface the reason as a transient status toast.
            dispatch(StreamError{std::string{"models fetch: "} + e.what()});
            dispatch(ModelsLoaded{std::vector<ModelInfo>{}});
        } catch (...) {
            dispatch(StreamError{"models fetch: unknown exception"});
            dispatch(ModelsLoaded{std::vector<ModelInfo>{}});
        }
    });
}

Cmd<Msg> open_browser_async(std::string url) {
    // task_isolated rather than task: posix_spawn / ShellExecute can
    // wedge on a hung WindowServer or a bizarre default-opener.
    // Isolated thread keeps a wedge from starving the shared BG pool.
    return Cmd<Msg>::task_isolated([url = std::move(url)]
                                   (std::function<void(Msg)>) {
        // No dispatch — the reducer doesn't care whether the browser
        // launched. The user can always paste auth_url manually from
        // the modal if their default opener is broken.
        auth::open_browser(url);
    });
}

Cmd<Msg> oauth_exchange(auth::OAuthCode    code,
                        auth::PkceVerifier verifier,
                        auth::OAuthState   state) {
    return Cmd<Msg>::task(
        [code = std::move(code),
         verifier = std::move(verifier),
         state = std::move(state)]
        (std::function<void(Msg)> dispatch) {
            try {
                auto r = auth::exchange_code(code, verifier, state);
                dispatch(LoginExchanged{std::move(r)});
            } catch (const std::exception& e) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"exchange threw: "} + e.what()})});
            } catch (...) {
                dispatch(LoginExchanged{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "exchange threw: unknown exception"})});
            }
        });
}

Cmd<Msg> load_threads_async() {
    // task_isolated rather than task: the JSON parse can take seconds
    // on a deep history (the directory walk is `directory_iterator` +
    // `nlohmann::json::parse` per file). Isolating it keeps the shared
    // worker pool free for stream / tool tasks the user fires in the
    // meantime.
    return Cmd<Msg>::task_isolated([](std::function<void(Msg)> dispatch) {
        try {
            auto threads = deps().load_threads();
            dispatch(ThreadsLoaded{std::move(threads)});
        } catch (...) {
            // Best-effort: a corrupt file is already logged + skipped
            // by load_all_threads, so any throw here is something the
            // user can't act on. Dispatch an empty list so the UI
            // doesn't sit on "loading…" forever.
            dispatch(ThreadsLoaded{std::vector<Thread>{}});
        }
    });
}

Cmd<Msg> load_thread_async(ThreadId id) {
    // task_isolated rather than task: a single big thread (multi-MB,
    // hundreds of messages) still takes 20-50ms of synchronous parse,
    // small enough to keep on the worker pool — but isolating matches
    // the load_threads_async policy and keeps the per-thread parse
    // off the same pool that tools/stream contend for.
    return Cmd<Msg>::task_isolated(
        [id = std::move(id)](std::function<void(Msg)> dispatch) {
            try {
                auto loaded = deps().load_thread(id);
                if (loaded) {
                    dispatch(ThreadLoaded{std::move(*loaded)});
                } else {
                    // Disk read / parse failure: surface an empty
                    // Thread so the reducer clears `thread_loading`
                    // and the UI doesn't sit stuck on the spinner.
                    // Reducer detects empty ThreadId == no swap.
                    dispatch(ThreadLoaded{Thread{}});
                }
            } catch (...) {
                dispatch(ThreadLoaded{Thread{}});
            }
        });
}

Cmd<Msg> refresh_oauth(std::string refresh_token) {
    return Cmd<Msg>::task(
        [refresh_token = std::move(refresh_token)]
        (std::function<void(Msg)> dispatch) {
            try {
                auto r = auth::refresh_access_token(
                    auth::RefreshToken{refresh_token});
                dispatch(TokenRefreshed{std::move(r)});
            } catch (const std::exception& e) {
                dispatch(TokenRefreshed{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    std::string{"refresh threw: "} + e.what()})});
            } catch (...) {
                dispatch(TokenRefreshed{std::unexpected(auth::OAuthError{
                    auth::OAuthErrorKind::Network,
                    "refresh threw: unknown exception"})});
            }
        });
}

} // namespace agentty::app::cmd
