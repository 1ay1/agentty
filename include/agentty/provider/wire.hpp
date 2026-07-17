#pragma once
// agentty::provider::wire — the shared byte-level stream framing that every
// transport (Anthropic SSE, OpenAI-compat SSE, Ollama native NDJSON) used to
// re-implement line-for-line.
//
// Before this header, three transports each carried their own copy of the
// exact same loop:
//
//     buf.append(data, len);
//     while ((nl = buf.find('\n', read_pos)) != npos) {
//         line = buf.substr(read_pos, nl - read_pos);
//         read_pos = nl + 1;
//         if (line.back() == '\r') line.remove_suffix(1);
//         ...dispatch line...
//     }
//     if (read_pos >= kCompactThreshold) { buf.erase(0, read_pos); read_pos = 0; }
//
// plus, for the two SSE transports, an identical `data:` / `event:`
// accumulator with the same 4 MiB overflow cap. A fix to any of that (a
// buffer-compaction bug, an overflow-cap tweak, a `\r\n` edge case) had to
// land in three places or silently diverge. Now it lives once, here.
//
// SCOPE — deliberately narrow. This owns ONLY the transport-agnostic framing:
// splitting a byte stream into lines, and (for SSE) grouping lines into
// events. It knows nothing about JSON, tool calls, salvage, usage frames, or
// StopReason — all of that stays in each transport's own StreamCtx, which is
// genuinely provider-specific and is NOT touched by this refactor. The framer
// is a plumbing layer the transport drives with its own dispatch callback.
//
// Header-only + templated on the callback: framing is the hottest inner loop
// of a live stream, so the per-line dispatch must inline. No virtuals, no
// std::function on the hot path.

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace agentty::provider::wire {

// Trailing readable bytes SseFramer keeps live past the end of every
// dispatched `data:` payload. Set equal to simdjson::SIMDJSON_PADDING (64) so
// a transport can hand the accumulator straight to simdjson's ondemand parser
// WITHOUT the per-frame memcpy-into-a-padded-scratch dance — simdjson only
// requires the padding bytes be *readable* (SIMD over-reads tolerate junk),
// which the reserved accumulator satisfies. Kept as a local constant so
// wire.hpp stays free of a simdjson include; a static_assert in the transport
// pins it to the real SIMDJSON_PADDING.
inline constexpr std::size_t kSseSimdPadding = 64;

// Compact `buf` (erasing the already-consumed prefix) only once `read_pos`
// crosses this — turns the per-chunk drain from O(buffered bytes) to
// amortized O(1) on a hot stream, since the in-flight tail past the last
// line boundary is always small.
inline constexpr std::size_t kDefaultCompactThreshold = 64 * 1024;

// Hard ceiling on a single SSE event's multi-line `data:` accumulator. Real
// frames are 1-10 KB; 4 MiB is generously past the largest content_block_delta
// ever observed. A misbehaving server streaming an unbounded `data:` would
// otherwise fill the accumulator until OOM — when the cap trips we drop the
// in-flight event and resync on the next blank line.
inline constexpr std::size_t kDefaultSseDataAccumMax = 4 * 1024 * 1024;

// ── Line framer ──────────────────────────────────────────────────────────
//
// Splits an incoming byte stream into `\r?\n`-terminated lines. Owns the
// growth buffer + read cursor; the caller owns all semantic state and
// receives each complete line (trailing `\r` already stripped, terminator
// excluded) via the callback. The final unterminated partial line stays
// buffered for the next feed().
//
// Used directly by NDJSON transports (Ollama /api/chat). SSE transports drive
// it through SseFramer below.
class LineFramer {
public:
    explicit LineFramer(std::size_t compact_threshold = kDefaultCompactThreshold)
        : compact_threshold_(compact_threshold) {
        buf_.reserve(64 * 1024);
    }

    // Append `len` bytes and dispatch every newly-complete line. `on_line` is
    // invoked as `on_line(std::string_view)`; the view is valid only for the
    // duration of the call (it points into the internal buffer, which may be
    // compacted afterward).
    template <class OnLine>
    void feed(const char* data, std::size_t len, OnLine&& on_line) {
        buf_.append(data, len);
        std::string_view buf{buf_};
        while (true) {
            const auto nl = buf.find('\n', read_pos_);
            if (nl == std::string_view::npos) break;
            std::string_view line = buf.substr(read_pos_, nl - read_pos_);
            read_pos_ = nl + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            on_line(line);
        }
        if (read_pos_ >= compact_threshold_) {
            buf_.erase(0, read_pos_);
            read_pos_ = 0;
        }
    }

    // Reserve extra buffer headroom (transports that know their event sizes
    // pre-reserve to dodge a realloc cascade on a fast stream).
    void reserve(std::size_t n) { buf_.reserve(n); }

private:
    std::string buf_;
    std::size_t read_pos_ = 0;
    std::size_t compact_threshold_;
};

// ── SSE event framer ─────────────────────────────────────────────────────
//
// Layers the Server-Sent-Events grouping rules on top of LineFramer:
//   • `event: <name>` sets the current event name,
//   • `data: <payload>` appends to the current event's data accumulator
//     (multi-line data joined by '\n', per the SSE spec),
//   • a blank line terminates the event and dispatches (name, data),
//   • `:` comments and unknown fields are silently dropped.
//
// A single event whose accumulated `data:` exceeds `data_accum_max` is
// dropped: the accumulator is cleared and subsequent lines for that event are
// ignored until the blank-line terminator resyncs. `on_event` is only called
// for non-empty events (either a name or some data present), matching what
// each transport did by hand.
//
// The framer does NOT interpret the data — it hands the raw accumulated
// payload back so Anthropic can dispatch on event name and OpenAI can look
// for `[DONE]` / parse JSON, exactly as before.
class SseFramer {
public:
    explicit SseFramer(std::size_t compact_threshold = kDefaultCompactThreshold,
                       std::size_t data_accum_max     = kDefaultSseDataAccumMax)
        : lines_(compact_threshold), data_accum_max_(data_accum_max) {
        // Reserve payload headroom PLUS the simd padding tail so the common
        // single-line `data:` frame never reallocates and the accumulator's
        // storage always has kSseSimdPadding readable bytes past the payload.
        data_accum_.reserve(8 * 1024 + kSseSimdPadding);
    }

    // Feed raw bytes; dispatch every complete event. `on_event` is invoked as
    // `on_event(std::string_view event_name, std::string_view data,
    //           char* padded_data)`. All three refer to the same payload:
    // `data` is the read-only view; `padded_data` points at the SAME bytes in
    // the accumulator's storage but guarantees kSseSimdPadding readable
    // trailing bytes (for an in-place simdjson ondemand parse with no
    // memcpy). `padded_data` is nullptr only for the degenerate empty payload.
    // All are valid only for the duration of the call.
    template <class OnEvent>
    void feed(const char* data, std::size_t len, OnEvent&& on_event) {
        lines_.feed(data, len, [&](std::string_view line) {
            if (line.empty()) {
                if (!skip_event_ && (!data_accum_.empty() || !event_name_.empty())) {
                    // Ensure kSseSimdPadding readable bytes past the payload.
                    // reserve() guarantees capacity; the bytes in [size,
                    // capacity) are readable (they belong to the allocation),
                    // which is all simdjson's SIMD over-read requires. The
                    // reserve is a no-op on the hot path once the accumulator
                    // has grown to its steady-state size.
                    if (data_accum_.capacity() < data_accum_.size() + kSseSimdPadding)
                        data_accum_.reserve(data_accum_.size() + kSseSimdPadding);
                    char* padded = data_accum_.empty()
                        ? nullptr : data_accum_.data();
                    on_event(std::string_view{event_name_},
                             std::string_view{data_accum_},
                             padded);
                }
                event_name_.clear();
                data_accum_.clear();
                skip_event_ = false;
                return;
            }
            if (skip_event_) return;

            if (line.starts_with("event:")) {
                std::size_t s = 6;
                while (s < line.size() && line[s] == ' ') ++s;
                event_name_.assign(line.data() + s, line.size() - s);
            } else if (line.starts_with("data:")) {
                std::size_t s = 5;
                while (s < line.size() && line[s] == ' ') ++s;
                const std::size_t add = (line.size() - s)
                    + (data_accum_.empty() ? 0 : 1);
                if (data_accum_.size() + add > data_accum_max_) {
                    // Overflow: drop the in-flight event and skip the rest of
                    // its data lines until the blank-line terminator, so
                    // partial bytes can't accumulate fresh and dispatch as a
                    // corrupted event.
                    data_accum_.clear();
                    event_name_.clear();
                    skip_event_ = true;
                    return;
                }
                if (!data_accum_.empty()) data_accum_.push_back('\n');
                data_accum_.append(line.data() + s, line.size() - s);
            }
            // `:` comments and unknown fields silently dropped (SSE spec).
        });
    }

    void reserve(std::size_t n) { lines_.reserve(n); }

private:
    LineFramer  lines_;
    std::string event_name_;
    std::string data_accum_;
    bool        skip_event_ = false;
    std::size_t data_accum_max_;
};

// ── Tool-result wire budget (age-tiered "tool result clearing") ───────────
//
// SINGLE SOURCE OF TRUTH for how much of a tool's output ships on the wire.
// Every transport (Anthropic, OpenAI-compat, Ollama native) used to size tool
// results differently — Anthropic had the full head+tail cap + age-tiering;
// the others shipped the raw output with only a coarse global scrub cap. A
// giant `read`/`grep`/`bash` dump therefore replayed IN FULL on every
// subsequent turn on the OpenAI/Ollama paths, burning context on the very
// models (local + smaller-window) that can least afford it. Now the policy
// lives once, here, and all three transports call it.
//
// The full TRANSCRIPT is never touched — only the WIRE copy of each tool
// result is sized by (a) its byte length and (b) how RECENT the call is.
// Rationale, from Anthropic's context-engineering guidance: "once a tool has
// been called deep in the message history, why would the agent need to see
// the raw result again?" Claude Code keeps the most-recent results at full
// fidelity and fades the rest.
//
//   rank 0 .. kFullResultWindow-1  → full budget (active working set)
//   rank >= kFullResultWindow      → tight head+tail (recall WHAT it returned,
//                                    not replay it)
//
// `recency_rank` is 0 for the newest terminal tool result in the thread and
// grows toward the oldest. Two invariants keep this loss-free for reasoning:
//   • ERROR results are NEVER faded — the model needs the full failure text
//     to recover, however old.
//   • Results already smaller than the faded budget ship verbatim regardless
//     of age — there's nothing to gain by touching them (cap_tool_result is a
//     no-op under budget).

inline constexpr std::size_t kToolResultFullBudget  = 64u * 1024u;
inline constexpr std::size_t kToolResultFadedBudget = 2u  * 1024u;
inline constexpr int         kFullResultWindow      = 8;

// Back up an index to the start of the UTF-8 code point it lands in. `i` is a
// prospective cut point; if it falls inside a multi-byte sequence we walk left
// until a lead byte (or 0). Bounded at 3 continuation bytes (max UTF-8
// sequence is 4 bytes).
[[nodiscard]] inline std::size_t utf8_floor(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size()) return s.size();
    std::size_t steps = 0;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80 && steps < 3) {
        --i; ++steps;
    }
    return i;
}

// Advance an index forward to the next UTF-8 code-point boundary.
[[nodiscard]] inline std::size_t utf8_ceil(std::string_view s, std::size_t i) noexcept {
    std::size_t steps = 0;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80 && steps < 3) {
        ++i; ++steps;
    }
    return i;
}

// Wire-only byte-budget cap for a single tool result. A 500 KiB grep or a
// `read` of a giant file otherwise bloats EVERY subsequent request (the result
// replays on each turn) until auto-compaction eventually kicks in. Mirrors
// Zed's take_text_within_byte_budget (thread.rs): cap at a byte budget, cut on
// a UTF-8 boundary, splice a marker so the model knows bytes were elided. We
// keep head + tail (not just head) because tool output often carries the
// operative summary or error at the END (compiler tally, "N matches", exit
// status). Transcript is untouched — the user still sees the full output; only
// the wire copy is trimmed.
[[nodiscard]] inline std::string cap_tool_result(std::string_view in, std::size_t budget) {
    if (in.size() <= budget) return std::string{in};

    const std::size_t elided = in.size() - budget;
    std::string marker = "\n\n...[" + std::to_string(elided)
                       + " bytes elided to fit wire budget; full output is "
                         "in the transcript]...\n\n";

    // Pathologically small budget: hard-truncate the head on a boundary.
    if (marker.size() + 16 >= budget) {
        return std::string{in.substr(0, utf8_floor(in, budget))};
    }

    const std::size_t body_budget = budget - marker.size();
    const std::size_t head_len    = (body_budget * 7) / 10;
    const std::size_t tail_len    = body_budget - head_len;

    const std::size_t head_cut  = utf8_floor(in, head_len);
    const std::size_t tail_from = utf8_ceil(in, in.size() - tail_len);

    std::string out;
    out.reserve(head_cut + marker.size() + (in.size() - tail_from));
    out.append(in.substr(0, head_cut));
    out.append(marker);
    out.append(in.substr(tail_from));
    return out;
}

// The wire budget for a tool result at the given recency rank. Errors keep the
// full budget at any age; recent results keep it; stale successful results
// fade to the tight head+tail budget.
[[nodiscard]] inline std::size_t faded_tool_budget(int recency_rank, bool is_error) noexcept {
    const bool faded = !is_error && recency_rank >= kFullResultWindow;
    return faded ? kToolResultFadedBudget : kToolResultFullBudget;
}

// Convenience: cap a tool result by its age. One call every transport shares —
// pick the budget from (recency_rank, is_error), then head+tail-cap to it.
[[nodiscard]] inline std::string cap_tool_result_aged(std::string_view raw,
                                                      int recency_rank,
                                                      bool is_error) {
    return cap_tool_result(raw, faded_tool_budget(recency_rank, is_error));
}

} // namespace agentty::provider::wire
