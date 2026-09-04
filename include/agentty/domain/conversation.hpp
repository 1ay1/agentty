#pragma once
// agentty conversation domain — the pure value types that describe a chat.
//
// No I/O, no UI, no streaming state machine.  A `Thread` is what gets
// persisted, sent to the provider, and displayed.  `Message` and `ToolUse`
// are its building blocks.

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/domain/id.hpp"
#include "agentty/domain/rag_mode.hpp"
#include "agentty/runtime/composer_attachment.hpp"

namespace agentty {

enum class Role : std::uint8_t { User, Assistant, System };

[[nodiscard]] constexpr std::string_view to_string(Role r) noexcept {
    switch (r) {
        case Role::User:      return "user";
        case Role::Assistant: return "assistant";
        case Role::System:    return "system";
    }
    return "?";
}

// A vision image carried by a User message OR a tool_result. Bytes are held
// RAW (not base64) so the in-memory Thread stays compact; encoding happens at
// the JSON write boundary. Persisted on disk as base64 so a loaded thread can
// be re-sent on a follow-up turn without re-reading the source file.
struct ImageContent {
    std::string media_type;  // "image/png", "image/jpeg", "image/webp", "image/gif"
    std::string bytes;       // raw image bytes, NOT base64
};

// `ToolUse::Status` is a sum type. Each alternative owns the data that is
// actually meaningful in that state — `Running` holds the live progress
// buffer, `Done`/`Failed` hold the final output, terminal states hold the
// finish time, etc. Storing those fields inline on `ToolUse` (the previous
// design) meant every reader had to remember which fields were valid in
// which state — variant alternatives make that invariant unbreakable.
//
// Wall-clock stamps use steady_clock (not system_clock) so a user changing
// the system clock mid-execution doesn't produce negative elapsed times.
struct ToolUse {
    // Pending carries a started_at because the card shows a live elapsed
    // counter during the args-streaming window too — Anthropic streams the
    // tool input as deltas and a long `content` field can take seconds, so
    // freezing the timer until execution begins reads as "stuck". The
    // timestamp survives the Pending → Running transition (kick_pending_tools
    // reads it via started_at()).
    struct Pending  { std::chrono::steady_clock::time_point started_at{}; };
    struct Approved { std::chrono::steady_clock::time_point started_at{}; };
    struct Running {
        std::chrono::steady_clock::time_point started_at{};
        // Live stdout+stderr snapshot for a running tool. Shown in the card
        // while status is Running so the user sees progress immediately
        // instead of waiting until the whole command finishes.
        std::string progress_text;
        // Wall-clock of the most recent progress snapshot. The hung-syscall
        // wedge net (update/meta.cpp Tick) measures liveness from
        // max(started_at, last_progress_at) instead of started_at alone, so a
        // long-but-HEALTHY tool that keeps emitting progress (a subagent
        // churning through tool calls, a multi-minute build streaming output)
        // is never guillotined by the flat wedge cap — only a tool that has
        // gone genuinely silent past the cap trips it. Zero until the first
        // ToolExecProgress lands; the wedge net treats zero as "no progress
        // yet" and falls back to started_at.
        std::chrono::steady_clock::time_point last_progress_at{};
    };
    struct Done {
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
        std::string output;
        // Images a tool chose to surface to a vision model (e.g. `read` on an
        // image file). Raw bytes; the wire encodes them as image blocks inside
        // this call's tool_result, right after the text. Empty for text tools.
        std::vector<ImageContent> images;
    };
    struct Failed {
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
        std::string output;
    };
    struct Rejected {
        std::chrono::steady_clock::time_point finished_at{};
    };
    using Status = std::variant<Pending, Approved, Running, Done, Failed, Rejected>;

    ToolCallId     id;
    ToolName       name;
    nlohmann::json args;
    std::string    args_streaming;
    // Throttle for the live preview re-parse during input_json_delta. The
    // preview path closes the partial JSON and runs `nlohmann::json::parse`
    // on the entire growing buffer to extract long fields (write `content`,
    // edit `edits[*].old_text`/`new_text`); doing that on every tiny delta
    // is O(n²) and was the dominant CPU cost on a multi-KB write — visible
    // as the UI "hanging" while the wire is healthy. Reducer skips the
    // preview re-parse if less than ~250 ms has passed since the last one.
    std::chrono::steady_clock::time_point last_preview_at{};
    // Byte offset into `args_streaming` where the opening `"` of the
    // streaming long-string field's *value* begins — i.e. just past
    // `"content":"` for write / `"command":"` for bash. Once we've
    // located it, subsequent preview ticks resume decoding from here
    // instead of re-scanning the full buffer from byte 0 every time.
    // Append-only growth of args_streaming keeps the offset valid.
    // 0 means "not located yet"; the offset is always > 0 when set
    // because the field name + `":"` is at least 4 bytes.
    std::size_t    stream_sniff_offset = 0;
    // Cached end-of-buffer size at the last preview pass. If the buffer
    // hasn't grown since then, there is nothing new to show — skip the
    // sniff + set_arg pair entirely. Cheap "am I still the same?" check
    // that eliminates the bulk of tail-identical re-renders when the
    // model pauses mid-stream.
    std::size_t    stream_sniff_size   = 0;
    // Incremental decode cache for the streaming long-string value
    // (write's `content`, future bash `command` if it grows large, etc).
    // Walks args_streaming exactly once across the tool's lifetime
    // instead of re-decoding [stream_sniff_offset, end) on every delta.
    // Cumulative cost drops from O(N²) to O(N) over the stream.
    //
    // stream_decoded_value holds the decoded preview tail (capped — see
    // kStreamingPreviewCap); the prefix is trimmed once the tail exceeds
    // 2× cap so memory stays bounded even on 10 MB writes.
    // stream_decode_through tracks the byte position in args_streaming
    // we've consumed; the next decode pass resumes there.
    mutable std::string stream_decoded_value;
    std::size_t         stream_decode_through = 0;
    // Amortization cursor for the edit/todo structured preview. Those two
    // branches can't use write's incremental cached-offset decode (they
    // mirror a growing ARRAY of objects, not one long string), so they lean
    // on try_parse_partial(args_streaming), which is O(|args_streaming|) per
    // call. Running it every ~120 ms tick is quadratic over a large multi-
    // edit call. Instead we re-parse only once the buffer has grown by
    // kStreamParseGrowth bytes since the last parse — the preview lags by at
    // most that many bytes (imperceptible) but the parse COUNT drops from
    // one-per-tick to one-per-growth-window, cutting the cumulative cost by
    // that factor. Holds the args_streaming size at the last structured
    // parse; 0 = never parsed.
    std::size_t         stream_parse_through = 0;
    // Set when StreamToolUseEnd / finalize_turn detected that the
    // wire ended inside a string value. finalize_turn's retry loop
    // treats this exactly like a missing-required-field truncation:
    // pop the in-flight assistant placeholder and silently relaunch
    // on the same ctx (bounded by kMaxTruncationRetries). Only after
    // the retry budget is exhausted does the tool surface as Failed.
    // Streaming-time scratch only — not persisted; default-init on
    // load is correct.
    bool           stream_mid_string_truncated = false;
    Status         status   = Pending{};

    // ── State predicates ─────────────────────────────────────────────────
    [[nodiscard]] bool is_pending()  const noexcept { return std::holds_alternative<Pending>(status);  }
    [[nodiscard]] bool is_approved() const noexcept { return std::holds_alternative<Approved>(status); }
    [[nodiscard]] bool is_running()  const noexcept { return std::holds_alternative<Running>(status);  }
    [[nodiscard]] bool is_done()     const noexcept { return std::holds_alternative<Done>(status);     }
    [[nodiscard]] bool is_failed()   const noexcept { return std::holds_alternative<Failed>(status);   }
    [[nodiscard]] bool is_rejected() const noexcept { return std::holds_alternative<Rejected>(status); }
    [[nodiscard]] bool is_terminal() const noexcept { return is_done() || is_failed() || is_rejected(); }

    // Exhaustiveness pin for is_terminal(). The freeze gate
    // (run_is_freezable) and the live-tail cache-key gate decide "is
    // this run safe to freeze into immutable scrollback?" by calling
    // is_terminal(). That
    // predicate enumerates the three settled states (Done/Failed/Rejected)
    // by exclusion of the three in-flight ones (Pending/Approved/Running).
    // A 7th Status variant added without classifying it here would default
    // to non-terminal silently — at best a run that never freezes (lag),
    // at worst, if classified terminal-by-accident elsewhere, a spinner
    // pinned in scrollback forever. Pin the width so the omission is a
    // build error, not a runtime ghost.
    static_assert(std::variant_size_v<Status> == 6,
                  "ToolUse::Status gained/lost a variant — re-derive "
                  "is_terminal() (Done/Failed/Rejected) and classify the "
                  "new state as terminal or in-flight before bumping this.");

    // ── State-safe accessors ─────────────────────────────────────────────
    // Return the relevant field for the current state, or an empty/default
    // when the alternative doesn't carry one. Views can rely on these
    // without first checking the discriminator. Returning by const-ref
    // keeps existing call sites that did `.empty()` / `.substr(…)` on the
    // old field unchanged.
    [[nodiscard]] const std::string& output() const noexcept {
        static const std::string empty;
        if (auto* d = std::get_if<Done>(&status))   return d->output;
        if (auto* f = std::get_if<Failed>(&status)) return f->output;
        return empty;
    }
    // Images a completed tool surfaced (read on an image file). Empty for
    // every non-Done state and every text tool. The wire renders these as
    // image blocks inside this call's tool_result.
    [[nodiscard]] const std::vector<ImageContent>& done_images() const noexcept {
        static const std::vector<ImageContent> empty;
        if (auto* d = std::get_if<Done>(&status)) return d->images;
        return empty;
    }
    [[nodiscard]] const std::string& progress_text() const noexcept {
        static const std::string empty;
        if (auto* r = std::get_if<Running>(&status)) return r->progress_text;
        return empty;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept {
        return std::visit([](const auto& s) -> std::chrono::steady_clock::time_point {
            if constexpr (requires { s.started_at; }) return s.started_at;
            else return {};
        }, status);
    }
    [[nodiscard]] std::chrono::steady_clock::time_point finished_at() const noexcept {
        return std::visit([](const auto& s) -> std::chrono::steady_clock::time_point {
            if constexpr (requires { s.finished_at; }) return s.finished_at;
            else return {};
        }, status);
    }

    // String tag for serialization / logging. Stable across versions; the
    // reverse direction lives in persistence.cpp.
    [[nodiscard]] std::string_view status_name() const noexcept {
        return std::visit([](const auto& s) -> std::string_view {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::same_as<T, Pending>)       return "pending";
            else if constexpr (std::same_as<T, Approved>) return "approved";
            else if constexpr (std::same_as<T, Running>)  return "running";
            else if constexpr (std::same_as<T, Done>)     return "done";
            else if constexpr (std::same_as<T, Failed>)   return "failed";
            else                                          return "rejected";
        }, status);
    }

    // Lazy cache of args.dump() for the view. args.dump() is O(args) per
    // call and ran per-frame for tools without a bespoke renderer, which
    // made big tool_use streams O(frame × args²). Invalidate via
    // mark_args_dirty() whenever `args` is mutated.
    mutable std::string args_dump_cache;
    mutable bool        args_dump_valid = false;

    void mark_args_dirty() {
        args_dump_valid = false;
        args_dump_cache.clear();
    }
    const std::string& args_dump() const {
        if (!args_dump_valid) {
            args_dump_cache = args.dump();
            args_dump_valid = true;
        }
        return args_dump_cache;
    }

    // O(1) render key. Called once per visible tool every frame via
    // Message::compute_render_key → turn_element/turn_config cache
    // predicate. Hashing the full output bytes here meant frame time
    // grew O(total transcript bytes) on long sessions — a few large
    // Read/Bash outputs and the per-frame cost dominated. Output
    // bytes are append-only within a (status.index(), output.size())
    // tuple in practice: Running grows progress_text but is_terminal
    // is false (we don't cache); Done/Failed land their `output` once
    // and never mutate it after. A hypothetical re-execute replaces
    // the whole ToolUse (new ToolCallId, new MessageId-keyed cache
    // slot) so byte-level disambiguation isn't needed here.
    [[nodiscard]] std::uint64_t compute_render_key() const {
        std::uint64_t k = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ULL; };
        mix(output().size());
        mix(progress_text().size());
        mix(args_streaming.size());
        mix(static_cast<std::uint64_t>(status.index()));
        return k;
    }
};

struct Message {
    // Stable per-message identity. Generated on construction; round-
    // tripped through persistence so it survives reloads. The view's
    // render cache keys by (thread_id, message.id) rather than
    // (thread_id, msg_idx) — compaction, deletion, or reordering
    // therefore can't return a stale cached Element for a now-different
    // message at the same position. Default-init via new_message_id()
    // means EVERY Message is identifiable as soon as it exists.
    MessageId   id = new_message_id();
    Role        role = Role::User;
    std::string text;
    /// Image attachments on a User message — the bytes that the
    /// transport flattens into Anthropic image content blocks. Empty
    /// for Assistant messages and for User messages that didn't carry
    /// any image at submit time. Order matches the `[image: ...]`
    /// markers in `text` so the rendered prose still anchors the
    /// images visually.
    std::vector<ImageContent> images;
    /// Non-image attachments (Paste / FileRef / Symbol) preserved
    /// from the composer at submit time. `text` retains the chip-
    /// form placeholders (`\x01ATT:N\x01`); the renderer substitutes
    /// each placeholder with `attachment::chip_label(...)` so the
    /// transcript shows a compact pill instead of inlining a 400-
    /// line paste, and the transport calls `attachment::expand(...)`
    /// to splice the full body back in when serialising the wire
    /// payload. Image attachments are NOT stored here — their bytes
    /// ride on `images` above and are sent as Anthropic image
    /// content blocks; the chip in `text` still renders as a pill
    /// in the transcript via a chip-label substitution from the
    /// corresponding ImageContent's path.
    std::vector<Attachment>   attachments;
    std::string streaming_text;
    // The wire model id that ACTUALLY served this assistant turn, and the
    // Smart Mode role it was routed as. Empty = "whatever was selected"
    // (no Smart Mode, or a turn from before this field existed).
    //
    // Why this is stored per-message rather than read from Model::d.model_id
    // at render time: under Smart Mode the turn is dispatched on the
    // resolved ROLE model (`strategic_profile.model` in launch_stream), which
    // is frequently NOT the model in the picker. Rendering the header from
    // the live selection therefore lied — the badge said "Mistral" while
    // every byte came from GLM. It also lied retroactively: switching models
    // relabelled every turn already in the transcript. A turn's provenance is
    // a property OF THE TURN, so it lives on the turn.
    std::string served_model;
    // Which role slot produced it: "strategic" | "implementation" |
    // "utility". Drives the accent colour on the turn header so delegation
    // is visible at a glance. Empty when Smart Mode was off.
    std::string served_role;
    // ── Extended/adaptive thinking (Assistant turns only) ──────────────
    // When effort is on, the Claude provider enables adaptive thinking and
    // the model emits a leading `thinking` content block (text — usually
    // empty under the default `display:omitted` — plus an opaque
    // `signature`). Anthropic REQUIRES that block be replayed verbatim on
    // the follow-up turn that carries this assistant's tool_use, or it 400s
    // the request. We capture both here during streaming, serialise them as
    // the first content block of the assistant message on the wire, and
    // persist them so a reloaded thread can still be continued. Empty for
    // User turns and for Assistant turns produced without thinking.
    //
    // MULTI-BLOCK: with the interleaved-thinking beta (and on any wire that
    // emits several thinking blocks per response) ONE assistant message can
    // carry SEVERAL independently-signed blocks. Each signature covers its
    // own block's exact text, so merging them (concat texts + keep last
    // signature) corrupts every block and 400s the replay. `thinking_blocks`
    // keeps the (text, signature) pairs separate and IN ORDER — the
    // authoritative replay source. `thinking` remains the concatenated
    // DISPLAY text (blocks joined \n\n) and `thinking_signature` mirrors the
    // last block's signature for backward compat with older persisted
    // threads (wire replay falls back to the legacy pair when the vector is
    // empty).
    struct ThinkingBlock {
        std::string text;
        std::string signature;
        // Non-empty = this is a REDACTED thinking block: Anthropic's safety
        // system encrypted the whole block into an opaque `data` payload
        // (arrives complete in content_block_start). It must be replayed as
        // {"type":"redacted_thinking","data":…} before the tool_use it
        // precedes, or the follow-up 400s ("Expected thinking or
        // redacted_thinking but found tool_use"). Never rendered.
        std::string redacted_data;
    };
    std::vector<ThinkingBlock> thinking_blocks;
    std::string thinking;
    std::string thinking_signature;
    // Wall-clock reasoning duration for the "Reasoned · ~N tokens · 3.2s"
    // header meter. `reasoning_started_ms` is a transient steady-clock stamp
    // set on the first thinking delta (0 = not started, NOT persisted);
    // `reasoning_ms` is the finalized duration in milliseconds, sealed when
    // the first answer/tool output arrives or the stream ends, and IS
    // persisted so a reloaded thread still shows how long the turn thought.
    std::int64_t reasoning_started_ms = 0;
    std::int64_t reasoning_ms = 0;
    // ── Codex/Responses reasoning replay (Assistant turns only) ─────────
    // The Responses API is the OpenAI analogue of Anthropic's thinking
    // block. When we request `include:["reasoning.encrypted_content"]`, each
    // reasoning output item carries an OPAQUE `encrypted_content` blob. To
    // keep chain-of-thought across tool rounds under `store:false` (the
    // stateless Codex mode), that blob MUST be replayed as a `reasoning`
    // item in the next request's `input[]` — WITHOUT its server id (echoing
    // the id triggers a failing server-side lookup). We capture it here
    // during streaming, replay it ahead of this turn's function_call items,
    // and persist it so a reloaded thread stays continuable. `reasoning_summary`
    // is the human-visible summary text (shown as the thinking block); it is
    // NOT sent back (only encrypted_content is). Empty for User turns and for
    // Assistant turns produced without reasoning. Multiple reasoning items in
    // one turn are joined newline-separated in order.
    std::string reasoning_encrypted;
    std::string reasoning_summary;

    // Unified reasoning text for DISPLAY, across every provider. All three
    // reasoning wires funnel their VISIBLE text into `thinking` via
    // StreamThinkingDelta: Anthropic `thinking_delta`, Codex
    // `reasoning_summary_text.delta`, and OpenAI-compat `reasoning_content`
    // (DeepSeek / Grok / o-series). `reasoning_summary` is a legacy fallback
    // for any path that populated it directly. One accessor keeps the view,
    // the render key, and the toggle logic provider-agnostic (DRY): the UI
    // shows a reasoning block iff this is non-empty.
    [[nodiscard]] std::string_view reasoning_display_text() const noexcept {
        if (!thinking.empty())          return thinking;
        return reasoning_summary;
    }
    [[nodiscard]] bool has_reasoning() const noexcept {
        return !reasoning_display_text().empty();
    }
    // Smoothing buffer. Anthropic's SSE batches deltas at the server's
    // tokenizer rate — a single content_block_delta can carry 50+ chars,
    // and several can arrive in one TCP read. If we appended each
    // delta straight to `streaming_text` the user would see big jumps
    // every frame instead of the cursor-paced animation that makes
    // streaming feel alive.
    //
    // StreamTextDelta now appends to `pending_stream` instead. The
    // Tick handler drips bytes from `pending_stream` into
    // `streaming_text` at a rate that's fast enough to keep up with
    // realistic generation speeds (≥ 32 chars / 33 ms tick = ~960 c/s,
    // ~3× a typical Sonnet stream) while still revealing small
    // increments when chunks arrive in bursts.  The view renders
    // `streaming_text` exactly as before — the smoothing is invisible
    // to the renderer.
    std::string pending_stream;
    // Set the instant the wire closes this message's TEXT content block
    // (Anthropic content_block_stop for a text block) — which always
    // precedes the tool_use content_block_start. It is the authoritative
    // "the model is done typing prose" signal, and it arrives BEFORE any
    // tool card is pushed to tool_calls. The view reads it to pre-emptively
    // glide the reveal cursor to the live edge during that genuine quiet
    // gap, so the mandatory hard-snap when the card appears is already a
    // no-op → no visible reveal burst ("first char sticks then the rest
    // pops in with the next tool"). More robust than the byte-quiet timing
    // heuristic, which a fast wire out-runs. Sticky within a sub-turn;
    // reset when a fresh placeholder message is minted for post-tool prose.
    bool text_block_closed = false;
    std::vector<ToolUse> tool_calls;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    std::optional<CheckpointId> checkpoint_id;
    // Set when the turn ended in a stream-level error (overloaded, 5xx,
    // network drop, mid-stream parse failure, etc.). Carries just the
    // user-facing message — no "⚠" prefix or formatting; the view adds
    // those. Kept SEPARATE from `text` so the assistant's actual
    // partial output (preserved into `text` on error) and the failure
    // reason render distinctly. Status-bar banner reads
    // `m.s.status`; this field is the per-message inline copy.
    std::optional<std::string> error;
    // True for the synthetic User message that holds the compaction
    // summary at the head of a post-compact conversation. The view
    // renders a "Conversation compacted" divider above this message
    // (instead of the normal speaker rail) so the boundary is visible
    // in the transcript. The wire payload is unchanged — it goes to
    // the model as a normal User message carrying the summary text.
    // Mirrors Claude Code's `isCompactSummary` field on the synthesised
    // post-compact message (binary near offset 92759504).
    bool is_compact_summary = false;

    // Proactive RETRIEVED-CONTEXT injection, if this message is one.
    //
    // Set on the synthetic User message that carries PROACTIVELY RETRIEVED
    // context (SOTA "active retrieval", FLARE/Self-RAG family): when the
    // user submits a knowledge-shaped question, submit_message silently
    // runs the RAG pipeline and, only on a HIGH-confidence hit, inserts one
    // of these just before the assistant placeholder. On the wire it is a
    // normal User message whose text is a fenced <retrieved-context> block
    // the model can ground on; in the transcript the view renders a compact
    // one-line "retrieved context" affordance instead of the raw block.
    // Never shown as the user's own words, and skipped by the doom-loop /
    // run-start walk (not a real human turn boundary — see
    // agent_loop_should_break).
    //
    // This was three independent fields — `bool proactive_context`, plus a
    // `double proactive_confidence = -1.0` and a `bool proactive_expanded`
    // documented as "only meaningful when proactive_context is true". Three
    // fields describe eight states when only two shapes are real, and the
    // confidence carried a sentinel INSIDE its own domain: the field is a
    // probability in [0,1], so -1.0 was a value the type said was a
    // confidence and the reader had to know was not. Grouping them says it
    // once: absent = an ordinary message, engaged = these facts are real.
    //
    // The persisted shape is unchanged — it was already nested this way
    // (`if (proactive_context) { … confidence … }`), so on-disk the sum
    // type existed and only the in-memory type disagreed.
    struct ProactiveContext {
        // Retrieval confidence that gated the injection, from the RAG funnel
        // (ProactiveHit.confidence), so the transcript card can render a
        // real bar. Absent when the funnel did not report one. Wire-inert.
        std::optional<double> confidence;
        // View-only: show each source's FULL passage text instead of the
        // one-line snippet. Toggled by Ctrl+U (ToggleRetrievedExpanded) on
        // the newest such card. Wire-inert and NOT persisted — a display
        // preference that resets to collapsed on reload. Folded into
        // compute_render_key so flipping it invalidates the cached Element.
        bool expanded = false;
    };
    std::optional<ProactiveContext> proactive;

    // Is this a retrieved-context message? The question every consumer
    // actually asks; reads better than `.proactive.has_value()` at the ~8
    // sites that only need the boolean.
    [[nodiscard]] bool is_proactive_context() const noexcept {
        return proactive.has_value();
    }

    // Smart Mode routing telemetry (view-only, wire-inert, not persisted).
    // When `smart_routing` is set, this is a synthetic zero-text card that
    // renders the per-turn ROUTING DECISION as a first-class thread event
    // (🧠 "Smart Mode"): which model + effort the turn was routed to, the
    // classified complexity that scaled it, and which layers are active. It
    // is NOT a real user/assistant turn — skipped by every wire/loop walk
    // exactly like proactive_context. The actual subagent delegations render
    // as ordinary `task` tool cards; this card is the DECISION, they are the
    // execution.
    bool        smart_routing        = false;
    std::string smart_route_model;    // wire id the Strategic turn ran on
    std::string smart_route_effort;   // effort label ("off"/"high"/…)
    std::string smart_route_complexity; // "trivial"/"simple"/"standard"/"complex"
    std::string smart_route_note;     // effort PROVENANCE, e.g.
                                      // "medium → complex · learned +1 · session -1"
                                      // — makes the adaptive decision legible.
    bool        smart_route_orchestrate = false;  // delegation directive active
    bool        smart_route_subagents   = false;  // per-role subagent routing

    // Fork provenance card (view-visible, wire-VISIBLE). When `fork_note`
    // is set, this is a synthetic Role::User message seeded at the head of
    // a freshly-forked thread. Two jobs:
    //   1. VIEW: renders as a first-class "\u2443 Forked" event card (glyph +
    //      "Forked" label + a one-line body) so a brand-new fork is NOT a
    //      blank screen — the user sees, as a thread event, that the fork
    //      happened and that the parent transcript is readable on demand.
    //   2. WIRE: unlike smart_routing/proactive_context, this message IS
    //      sent to the model (its `text` is the transcript pointer). It is
    //      a User message on purpose — the ChatGPT/Responses transport
    //      DROPS mid-thread System messages (folded into `instructions`),
    //      so a System note could vanish and the model would never learn
    //      the parent transcript exists. A User message survives every
    //      provider path. `fork_transcript` holds the path for the card's
    //      body (the wire `text` carries the full instruction).
    bool        fork_note        = false;
    std::string fork_transcript;   // path to the parent transcript .md (may be empty)

    // FNV-1a over the fields that turn_element / turn_config consume
    // when building the rendered Element. The view cache stamps the
    // built Element with this key at insert time and re-checks it on
    // every hit; any mutation that changes a key-relevant field
    // (toggle expanded, tool output appended, status changed, error
    // attached, body bytes changed) bumps the key and forces a rebuild
    // instead of silently serving a stale cached Element back.
    //
    // pending_stream.size() is mixed in so a delta that lands only in
    // the Tick pacer's buffer (before it drips into streaming_text)
    // still advances the key. Without it the render gate
    // (program.hpp::visual_hash) sees an unchanged hash and skips the
    // frame; the live tail's reveal cursor, having caught up to
    // streaming_text, then stops re-arming its animation frame, so the
    // stream visibly freezes until an unrelated axis flips — the "md
    // streaming gets stuck" symptom.
    //
    // Keep this in sync with the actual reads in
    // src/runtime/view/thread/turn/turn.cpp.
    [[nodiscard]] std::uint64_t compute_render_key() const {
        std::uint64_t k = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ULL; };
        mix(static_cast<std::uint64_t>(role));
        mix(text.size());
        mix(streaming_text.size());
        mix(pending_stream.size());
        mix(images.size());
        mix(attachments.size());
        for (const auto& a : attachments) {
            // Body bytes don't change after submit; mixing size is
            // enough to invalidate the render cache on a hypothetical
            // future edit and cheap enough to keep here.
            mix(static_cast<std::uint64_t>(a.kind));
            mix(a.body.size());
            mix(a.path.size());
            mix(a.name.size());
            mix(static_cast<std::uint64_t>(a.line_count));
        }
        mix(tool_calls.size());
        for (const auto& tc : tool_calls) mix(tc.compute_render_key());
        mix(error ? error->size() + 1 : 0ULL);   // distinguish empty vs absent
        mix(is_compact_summary ? 1ULL : 0ULL);
        mix(proactive ? 2ULL : 0ULL);
        mix(proactive && proactive->expanded ? 4ULL : 0ULL);
        // Reasoning block: its text grows during streaming, and the block
        // switches from the live thought stream to a settled one-line summary
        // once the answer starts. Mixing the length + a "has answer body"
        // bit re-renders the cached Element across that transition. Uses the
        // unified accessor so all providers key identically.
        mix(reasoning_display_text().size());
        // Turn provenance drives the header label + role tag, so a message
        // whose served_model differs from another's must not reuse its
        // cached Element. Sizes suffice: the pair is written once, at
        // StreamStarted, and never mutates afterwards.
        mix(served_model.size());
        mix(served_role.size());
        if (smart_routing) {
            mix(8ULL);
            mix(smart_route_model.size());
            mix(smart_route_effort.size());
            mix(smart_route_complexity.size());
            mix(smart_route_note.size());
            mix((smart_route_orchestrate ? 16ULL : 0ULL)
                | (smart_route_subagents ? 32ULL : 0ULL));
        }
        if (fork_note) {
            mix(64ULL);
            mix(fork_transcript.size());
        }
        // Quantize confidence to a bar-relevant bucket so a card whose
        // confidence changed (re-injection) invalidates the cache, without
        // churning on float noise. Only proactive messages carry it.
        if (proactive && proactive->confidence)
            mix(static_cast<std::uint64_t>(*proactive->confidence * 100.0) + 1ULL);
        return k;
    }
};

struct Thread {
    ThreadId    id;
    std::string title;
    std::vector<Message> messages;
    // Provenance: if this thread was forked (branched) from another, the
    // parent thread's id. Empty for a normal thread. Persisted so the thread
    // list can show a "forked from …" hint and the history stays traceable.
    std::string forked_from;
    // Per-thread proactive-RAG override. Absent = inherit the global RAG
    // mode; engaged = this thread's own behaviour. Set by the fork picker.
    //
    // This was `int rag_mode_override = -1`, with the comment obliged to
    // spell out the encoding ("-1 inherit, 0 On, 1 FirstTurnOnly, 2 Off") —
    // which is the tell: a comment documenting which integers are legal is a
    // type that has not been written down. Consumers cast back to compare,
    // so a value outside the enum was representable and silently meant
    // "inherit". optional says both halves in the type: whether there is an
    // override at all, and — if so — that it is a RagMode and nothing else.
    //
    // The enum lives in domain/rag_mode.hpp rather than store/, because
    // store/store.hpp includes THIS header — depending on it from here would
    // be a cycle. It is vocabulary, not storage, so domain/ is where it
    // belongs; store:: keeps an alias so existing spellings still compile.
    std::optional<RagMode> rag_mode_override;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = std::chrono::system_clock::now();

    // Wire-only compaction records. Each entry says "upstream requests
    // built from this thread should replace messages[0..up_to_index)
    // with a single synthetic User message carrying `summary`."
    //
    // Compaction NEVER mutates `messages`: the user's transcript is
    // immutable across compaction events. The user keeps seeing every
    // turn they ever had; only the wire payload to the provider gets
    // the prefix collapsed into a summary. The view draws a divider
    // between messages[up_to_index-1] and messages[up_to_index] to
    // signal "the model no longer sees the turns above this line."
    //
    // Stacked compactions: when the user compacts twice, the second
    // record's `up_to_index` covers everything up to the second
    // boundary and its `summary` was generated by summarising
    // [first_summary_synth_user, messages[first.up_to_index..second.up_to_index]].
    // Wire substitution always reads the LATEST record — earlier ones
    // are retained for history/persistence and for future surfacing
    // (e.g. "this thread has been compacted 3 times") but are not
    // consulted on the hot path. Vector is chronological.
    struct CompactionRecord {
        std::size_t up_to_index = 0;     // covers messages[0..up_to_index)
        std::string summary;             // model output, <summary>…</summary> stripped
        std::chrono::system_clock::time_point created_at =
            std::chrono::system_clock::now();
    };
    std::vector<CompactionRecord> compactions;
};

// Local estimate of the prefix size (in tokens) that the NEXT request to
// the model would carry, computed from `thread.messages` directly. Used
// by the auto-compaction trigger as a *proactive* check before launching
// a stream — `Session::tokens_in` is a lagging signal (updated from the
// PRIOR turn's StreamUsage event), so a turn with heavy tool outputs can
// push the next request past context-max with no warning otherwise.
//
// Estimate is bytes / 3.5, with an additive ~1500-token charge per image
// attachment (Anthropic's image content blocks tokenize to a fixed cost
// independent of byte size). Conservative: code prose averages ~3.3 bytes
// per token and tool-call JSON envelopes are usually under that, so 3.5
// errs slightly toward over-counting which is the safe direction here —
// triggering compaction one turn early costs one round trip; missing the
// trigger costs the whole session ("no coming back").
[[nodiscard]] inline int estimate_prefix_tokens(const Thread& t) noexcept {
    constexpr double kBytesPerToken    = 3.5;
    constexpr int    kTokensPerImage   = 1500;
    std::size_t bytes = 0;
    int images = 0;
    for (const auto& m : t.messages) {
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
    auto from_bytes = static_cast<int>(static_cast<double>(bytes) / kBytesPerToken);
    return from_bytes + images * kTokensPerImage;
}

struct PendingPermission {
    ToolCallId  id;
    ToolName    tool_name;
    std::string reason;
};

} // namespace agentty
