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
    struct Running  {
        std::chrono::steady_clock::time_point started_at{};
        // Live stdout+stderr snapshot for a running tool. Shown in the card
        // while status is Running so the user sees progress immediately
        // instead of waiting until the whole command finishes.
        std::string progress_text;
    };
    struct Done {
        std::chrono::steady_clock::time_point started_at{};
        std::chrono::steady_clock::time_point finished_at{};
        std::string output;
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
    bool           expanded = true;

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
        mix(expanded ? 1ULL : 0ULL);
        return k;
    }
};

/// Image content attached to a User message. Serialized on the wire
/// as an Anthropic image content block — `{"type":"image","source":
/// {"type":"base64","media_type":"...","data":"..."}}`. Bytes are
/// stored raw in memory (NOT base64) so the in-RAM representation
/// stays compact; encoding happens at the JSON write boundary.
/// Persisted on disk as base64 so a loaded thread can be re-sent on
/// a follow-up turn without re-asking the user for the file.
struct ImageContent {
    std::string media_type;  // "image/png", "image/jpeg", "image/webp", "image/gif"
    std::string bytes;       // raw image bytes, NOT base64
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
    std::string thinking;
    std::string thinking_signature;
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
        return k;
    }
};

struct Thread {
    ThreadId    id;
    std::string title;
    std::vector<Message> messages;
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
