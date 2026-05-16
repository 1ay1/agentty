#include "agentty/runtime/app/cmd_factory.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

#include "agentty/auth/auth.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace agentty::app::cmd {

using maya::Cmd;

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
    provider::Request req;
    req.model         = m.d.model_id.value;
    req.system_prompt = provider::anthropic::default_system_prompt();

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
    if (m.s.compacting) {
        req.messages = wire_messages_for_compaction(m.d.current, m.s.context_max);
        // req.tools left empty — summarisation is text-only.
    } else {
        req.messages = wire_messages_for_impl(m.d.current);
        // Adaptive soft-trim: if the wire view is still over the
        // window (no compactions yet, or the latest one is stale
        // and the user has run many turns since), drop oldest raw
        // turns from the wire until it fits ~95% of context_max.
        // The transcript is NOT mutated — this only shapes what
        // ships on THIS request. Lets the agent keep working past
        // the threshold without yanking it for compaction; the
        // user can run /compact at their leisure for a strictly
        // better summary-based shape on subsequent turns.
        if (m.s.context_max > 0) {
            const int soft_ceiling = static_cast<int>(
                static_cast<double>(m.s.context_max) * 0.95);
            soft_trim_to_ceiling(req.messages, soft_ceiling);
        }
        // All tools, every profile. Gating is the policy layer's job
        // (`tool::DynamicDispatch::needs_permission`, called from
        // `kick_pending_tools`) — it inspects the tool's effects against
        // the active profile and decides Allow vs Prompt:
        //   Write   → all Allow  (autonomous)
        //   Ask     → ReadFs/Pure Allow; WriteFs/Exec/Net Prompt
        //   Minimal → only Pure Allow; everything else Prompt
        // Hiding tools from the wire here used to be the gate, but it
        // conflicted with what users expect from "Ask": that the model
        // can attempt anything, the *user* approves per-call. With the
        // filter in place, Ask just made write/edit/bash invisible (no
        // prompt could ever fire because the model never called them) and
        // Minimal sent zero tools at all (model became chat-only). The
        // policy layer is exhaustively static_asserted in policy.hpp; let
        // it own the decision so the three profiles read as users expect.
        for (const auto& t : tools::registry()) {
            req.tools.push_back({t.name.value, t.description, t.input_schema,
                                 t.eager_input_streaming});
        }
    }
    req.auth_header = deps().auth_header;
    req.auth_style  = deps().auth_style;

    // Mint a fresh cancel token per turn and stash it on the active
    // ctx so the Esc handler (Msg::CancelStream) can flip it. The
    // worker thread holds its own shared_ptr via req.cancel; storing
    // it inside the phase variant on the UI thread is safe — both
    // sides only ever load/store the atomic flag. Caller has already
    // transitioned us into an active phase by the time launch_stream
    // runs, so active_ctx is non-null here.
    req.cancel = std::make_shared<http::CancelToken>();
    if (auto* a = active_ctx(m.s.phase)) a->cancel = req.cancel;
    // Capture the token by value so the worker can compare against the
    // cancel-state at every dispatch boundary. Without this guard, a
    // cancelled worker's trailing events (the StreamError("cancelled")
    // unwound by the worker ~200 ms after Esc, in particular) flow into
    // the reducer's StreamError handler and run `active_ctx(m.s.phase)
    // ->cancel.reset()` — but the active phase may have been replaced
    // with a fresh ctx if the user typed and submitted a new message in
    // the meantime, which means we silently null out the *new* turn's
    // cancel token. Result: Esc on the new turn does nothing, the user
    // can't cancel anymore, and "agentty gets stuck — nothing works".
    auto cancel_for_guard = req.cancel;

    return Cmd<Msg>::task([req = std::move(req), cancel_for_guard]
                          (std::function<void(Msg)> dispatch) mutable {
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
        auto guarded = [dispatch, cancel_for_guard](Msg m) {
            if (cancel_for_guard && cancel_for_guard->is_cancelled()) return;
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

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;

    // Effect-based scheduler. When the assistant emits multiple tool
    // calls in one turn they all start Pending; maya's BG worker pool
    // runs Task cmds on independent threads, so any set of tools we
    // dispatch in this tick runs concurrently. `is_parallel_safe`
    // (agentty/tool/effects.hpp) encodes the capability rule: Pure /
    // ReadFs / Net compose freely; WriteFs and Exec demand exclusive
    // access. We accumulate `active_effects` across the sweep so each
    // candidate is checked against *everything* already running AND
    // the siblings we've just promoted within this same batch.
    //
    // Deferred (conflicted) tools stay Pending. When the blocking
    // tool's ToolExecOutput lands, update.cpp re-fires
    // kick_pending_tools and the now-unblocked siblings advance.
    tools::EffectSet active_effects;
    auto effects_of = [](const ToolName& n) -> tools::EffectSet {
        if (const auto* sp = tools::spec::lookup(n.value)) return sp->effects;
        // Unknown tool: treat as if it requires exclusive access so we
        // never parallelise something whose effects we can't reason about.
        return tools::EffectSet{{tools::Effect::Exec}};
    };
    for (const auto& tc : last.tool_calls)
        if (tc.is_running()) active_effects |= effects_of(tc.name);

    for (auto& tc : last.tool_calls) {
        // Approved: user already granted permission; advance it exactly
        // like a Pending-but-no-permission-needed tool, minus the
        // permission check. Keeps the effect-parallel gate as the single
        // source of truth for scheduling.
        const bool ready = tc.is_pending() || tc.is_approved();
        if (ready) {
            const bool needs_perm = tc.is_approved()
                ? false
                : tool::DynamicDispatch::needs_permission(tc.name.value, m.d.profile);
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
                // Effect-compatibility gate. Defer this tool if another
                // running sibling demands exclusive access or this tool
                // itself demands exclusive access while anything is
                // already running. kick_pending_tools re-fires on every
                // terminal ToolExecOutput, so deferred siblings advance
                // automatically without explicit requeueing.
                const auto want = effects_of(tc.name);
                if (!tools::is_parallel_safe(active_effects, want)) {
                    any_pending = true;
                    continue;
                }

                // started_at was stamped at StreamToolUseStart so the
                // timer covers the full card lifetime (args streaming +
                // execution). Preserve it as we move into Running.
                tc.status = ToolUse::Running{tc.started_at(), {}};
                active_effects |= want;
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
            // Used to: abort the sub-turn and force a compaction round
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

            // Tool results are going back to the model — a fresh sub-turn
            // is about to start on the same assistant message. This is a
            // natural slice point: tool-heavy turns can grow the transcript
            // several messages in one logical "turn" from the user's POV,
            // and the submit_message virtualization hook never sees those.
            // Slicing here keeps the live canvas bounded even inside a
            // single long multi-tool assistant turn.
            auto virt = detail::maybe_virtualize(m);
            // ExecutingTool → Streaming (post-tool sub-turn). Active
            // ctx flows through: cancel token's still alive (the
            // request is still open, sub-turn streams over the same
            // SSE), retry counters preserved.
            auto ctx = take_active_ctx(std::move(m.s.phase));
            m.s.phase = phase::Streaming{std::move(ctx).value()};
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            if (!virt.is_none()) cmds.push_back(std::move(virt));
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
            auto models = provider::anthropic::list_models(deps().auth_header, deps().auth_style);
            dispatch(ModelsLoaded{std::move(models)});
        } catch (const std::exception& e) {
            dispatch(StreamError{std::string{"models fetch: "} + e.what()});
        } catch (...) {
            dispatch(StreamError{"models fetch: unknown exception"});
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
