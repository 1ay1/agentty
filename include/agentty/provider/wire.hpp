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
#include <string>
#include <string_view>

namespace agentty::provider::wire {

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
        data_accum_.reserve(8 * 1024);
    }

    // Feed raw bytes; dispatch every complete event. `on_event` is invoked as
    // `on_event(std::string_view event_name, std::string_view data)`. Both
    // views are valid only for the duration of the call.
    template <class OnEvent>
    void feed(const char* data, std::size_t len, OnEvent&& on_event) {
        lines_.feed(data, len, [&](std::string_view line) {
            if (line.empty()) {
                if (!skip_event_ && (!data_accum_.empty() || !event_name_.empty()))
                    on_event(std::string_view{event_name_},
                             std::string_view{data_accum_});
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

} // namespace agentty::provider::wire
