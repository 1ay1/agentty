// agentty::provider::anthropic — SSE event parser.
//
// Extracted verbatim from transport.cpp (behavior guarded by
// anthropic_sse_golden_test). Owns the per-event dispatch: the simdjson
// content_block_delta fast path, the SSE event-kind closed sum + its
// compile-time bijection proof, dispatch_event, and feed_sse. StreamCtx and
// the shared dbg/debug_log logger live in sse.hpp so run_stream_sync (which
// stays in transport.cpp) can drive a stream. Pure parse — no request build,
// no network.

#include "agentty/provider/anthropic/sse.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <simdjson.h>

namespace agentty::provider::anthropic {

namespace {

using json = nlohmann::json;

// Fast path: content_block_delta dominates stream volume (one per output
// token).  simdjson's ondemand walks the bytes in-place, grabs the two
// strings we need, and returns without ever materialising a DOM.  Falls
// back to caller for anything unexpected (unknown delta.type).
// Returns true if the event was fully handled.
//
// `padded` points into the SseFramer's own accumulator, which guarantees
// wire::kSseSimdPadding readable trailing bytes (== SIMDJSON_PADDING), so we
// iterate in place with ZERO per-frame copy — the old memcpy-into-simd_scratch
// dance is gone. simd_scratch survives only as the simdjson parser's owned
// working buffer (ctx.simd_parser), not a payload copy.
//
// simdjson ondemand UNESCAPES strings in place, mutating `padded` (which
// aliases the caller's `data`). So the result is a tri-state, not a bool:
//
//   Unparseable  — iterate()/object-shape failed BEFORE any string was
//                  extracted, so `data` is still pristine and the caller may
//                  safely re-parse it with nlohmann.
//   Recognized   — a well-formed content_block_delta whose `type` we don't
//                  render (or couldn't read); already dropped, and `data` may
//                  be dirtied — the caller must NOT re-parse. Matches the
//                  nlohmann branch, which also silently ignores unknown types.
//   Handled      — emitted the delta; done.
//
// This also retires the old double-parse: a non-text/tool delta used to fall
// through to a second full nlohmann parse of the same bytes.
enum class FastDelta { Unparseable, Recognized, Handled };
static_assert(wire::kSseSimdPadding == simdjson::SIMDJSON_PADDING,
              "wire::kSseSimdPadding must equal simdjson::SIMDJSON_PADDING so "
              "SseFramer's accumulator tail is a valid simdjson pad region");
FastDelta dispatch_content_block_delta_fast(StreamCtx& ctx, std::string_view data,
                                            char* padded) {
    if (!padded) return FastDelta::Unparseable;
    const std::size_t cap = data.size() + simdjson::SIMDJSON_PADDING;

    simdjson::ondemand::document doc;
    if (ctx.simd_parser.iterate(padded, data.size(), cap).get(doc))
        return FastDelta::Unparseable;

    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return FastDelta::Unparseable;

    simdjson::ondemand::object delta;
    if (root["delta"].get_object().get(delta)) return FastDelta::Unparseable;

    // From here on simdjson may unescape into `padded`/`data`; never return
    // Unparseable past this point — a malformed delta is Recognized (dropped).
    std::string_view delta_type;
    if (delta["type"].get_string().get(delta_type)) return FastDelta::Recognized;

    if (delta_type == "text_delta") {
        std::string_view text;
        if (delta["text"].get_string().get(text)) return FastDelta::Recognized;
        ctx.sink(StreamTextDelta{std::string{text}});
        return FastDelta::Handled;
    }
    if (delta_type == "input_json_delta") {
        std::string_view partial;
        if (delta["partial_json"].get_string().get(partial)) return FastDelta::Recognized;
        ctx.sink(StreamToolUseDelta{
            ToolCallId{ctx.current_tool_id}, std::string{partial}});
        return FastDelta::Handled;
    }
    // Thinking blocks have nothing to render but they ARE proof that the
    // model is actively working. Bump the reducer's liveness clock via a
    // StreamHeartbeat — without this, a long reasoning pass (extended-
    // thinking models can go 60-120 s between visible deltas) trips the
    // reducer's stall watchdog and fires a spurious "stream stalled"
    // error even though the wire is healthy and the model is producing
    // thinking tokens we've chosen not to render.
    if (delta_type == "thinking_delta") {
        // Capture the reasoning text (usually empty under display:omitted)
        // so the block can be replayed next turn. Doubles as a liveness
        // heartbeat — the reducer bumps last_event_at on this Msg too.
        std::string_view text;
        if (delta["thinking"].get_string().get(text)) return FastDelta::Recognized;
        ++ctx.thinking_deltas;
        ctx.sink(StreamThinkingDelta{std::string{text}, {}});
        return FastDelta::Handled;
    }
    if (delta_type == "signature_delta") {
        std::string_view sig;
        if (delta["signature"].get_string().get(sig)) return FastDelta::Recognized;
        ++ctx.thinking_deltas;
        ctx.sink(StreamThinkingDelta{{}, std::string{sig}});
        return FastDelta::Handled;
    }
    // Well-formed delta, type we don't render (forward-compat). The nlohmann
    // branch drops these too — don't waste a second parse re-discovering that.
    return FastDelta::Recognized;
}

// ── SSE event-kind closed sum ─────────────────────────────────────
// Every event name Anthropic emits, plus an Unknown sentinel for the
// forward-compat case (Anthropic adds a new event before agentty knows
// about it — we drop it silently, not crash). The enum is the closed
// dispatch surface; the kSseEvents table is the single point of
// translation from wire string → enum. Adding a new event = add an
// arm + a row; the bijection proof at the bottom fails the build if
// the two ever desync.
//
// Previously: a long if/else if chain of `name == "..."` strcmps, with
// a new event silently falling off the end. Now: kind_of_event() does
// the lookup once, the dispatcher switches on the enum, and an Unknown
// arm is the explicit drop site.
enum class SseEventKind : std::uint8_t {
    Unknown,
    Ping,
    MessageStart,
    ContentBlockStart,
    ContentBlockDelta,
    ContentBlockStop,
    MessageDelta,
    MessageStop,
    Error,
};

struct SseEventSpec {
    SseEventKind     kind;
    std::string_view wire_name;
};

inline constexpr std::array kSseEvents = {
    SseEventSpec{SseEventKind::Ping,              "ping"},
    SseEventSpec{SseEventKind::MessageStart,      "message_start"},
    SseEventSpec{SseEventKind::ContentBlockStart, "content_block_start"},
    SseEventSpec{SseEventKind::ContentBlockDelta, "content_block_delta"},
    SseEventSpec{SseEventKind::ContentBlockStop,  "content_block_stop"},
    SseEventSpec{SseEventKind::MessageDelta,      "message_delta"},
    SseEventSpec{SseEventKind::MessageStop,       "message_stop"},
    SseEventSpec{SseEventKind::Error,             "error"},
};

[[nodiscard]] constexpr SseEventKind kind_of_event(std::string_view name) noexcept {
    for (const auto& s : kSseEvents)
        if (s.wire_name == name) return s.kind;
    return SseEventKind::Unknown;
}

// Compile-time bijection: every Kind arm (except Unknown) has exactly
// one row in the table. Adding a new Kind without a wire_name — or
// duplicating a wire_name — fails the build at the static_assert.
namespace sse_proofs {
consteval bool kinds_in_table() {
    constexpr SseEventKind kAll[] = {
        SseEventKind::Ping,
        SseEventKind::MessageStart,
        SseEventKind::ContentBlockStart,
        SseEventKind::ContentBlockDelta,
        SseEventKind::ContentBlockStop,
        SseEventKind::MessageDelta,
        SseEventKind::MessageStop,
        SseEventKind::Error,
    };
    if (std::size(kAll) != kSseEvents.size()) return false;
    for (auto k : kAll) {
        int hits = 0;
        for (const auto& s : kSseEvents) if (s.kind == k) ++hits;
        if (hits != 1) return false;
    }
    return true;
}
static_assert(kinds_in_table(),
              "SseEventKind and kSseEvents must be in bijection — every "
              "non-Unknown Kind needs exactly one row whose wire_name "
              "matches the Anthropic SSE event identifier");
consteval bool names_unique() {
    for (std::size_t i = 0; i < kSseEvents.size(); ++i)
        for (std::size_t j = i + 1; j < kSseEvents.size(); ++j)
            if (kSseEvents[i].wire_name == kSseEvents[j].wire_name) return false;
    return true;
}
static_assert(names_unique(), "duplicate wire_name in kSseEvents");
static_assert(kind_of_event("message_stop")     == SseEventKind::MessageStop);
static_assert(kind_of_event("who_knows")        == SseEventKind::Unknown);
} // namespace sse_proofs

void dispatch_event(StreamCtx& ctx, std::string_view name, std::string_view data,
                    char* padded) {
    if (data.empty() || data == "[DONE]") return;
    // dbg() format string is %s — copy through a small stack buffer only
    // when the debug log is actually enabled (debug_log() returns nullptr
    // otherwise, in which case dbg() short-circuits and `name` is never
    // touched). Avoids constructing a std::string per event in the hot path.
    if (debug_log()) {
        std::string name_owned{name};
        std::string data_owned{data};
        dbg("<< event=%s data=%s\n", name_owned.c_str(), data_owned.c_str());
    }

    // Hot path first — ~95% of events during a streaming turn. The
    // simdjson fast path parses in place over the framer's padded buffer.
    // Only a genuinely UNPARSEABLE frame (parse error before any string was
    // touched, so `data` is still pristine) falls through to nlohmann;
    // Handled and Recognized (well-formed but unrendered type) both return
    // — re-parsing a delta simdjson already understood is pure waste.
    if (name == "content_block_delta") {
        switch (dispatch_content_block_delta_fast(ctx, data, padded)) {
            case FastDelta::Handled:
            case FastDelta::Recognized: return;
            case FastDelta::Unparseable: break;   // fall through to nlohmann
        }
    }

    const SseEventKind kind = kind_of_event(name);

    // ping events are heartbeat keepalives — Anthropic interleaves them
    // so proxies don't kill the long-poll (typically every 10-15 s).
    // Forward as a StreamHeartbeat so the reducer's stall watchdog can
    // tell "wire is silent but alive" from "wire is wedged." The
    // reducer's handler only bumps last_event_at — no render, no state.
    if (kind == SseEventKind::Ping) { ctx.sink(StreamHeartbeat{}); return; }

    json j;
    try { j = json::parse(data); } catch (...) { return; }

    switch (kind) {
        case SseEventKind::Unknown:
            // Forward-compat sink: a new event Anthropic added that we
            // don't know yet. Drop silently — the wire stays parseable,
            // and a future update either adds an arm or learns the
            // model needs a behavioural change. Logged via dbg() above.
            break;

        case SseEventKind::Ping:
            // Handled in the fast path above; unreachable in the switch.
            break;

        case SseEventKind::MessageStart: {
            ctx.sink(StreamStarted{});
            if (j.contains("message") && j["message"].contains("usage")) {
                const auto& u = j["message"]["usage"];
                StreamUsage su;
                su.input_tokens                = u.value("input_tokens", 0);
                su.output_tokens               = u.value("output_tokens", 0);
                su.cache_creation_input_tokens = u.value("cache_creation_input_tokens", 0);
                su.cache_read_input_tokens     = u.value("cache_read_input_tokens", 0);
                ctx.sink(su);
            }
            break;
        }

        case SseEventKind::ContentBlockStart: {
            auto block = j.value("content_block", json::object());
            auto type = block.value("type", "");
            if (type == "tool_use") {
                ctx.current_tool_id = block.value("id", "");
                ctx.current_tool_name = block.value("name", "");
                ctx.in_tool_use = true;
                ctx.sink(StreamToolUseStart{ToolCallId{ctx.current_tool_id}, ToolName{ctx.current_tool_name}});
            } else if (type == "text") {
                // Mark the text block open so its matching
                // content_block_stop emits StreamTextBlockClosed.
                ctx.text_block_open = true;
            }
            break;
        }

        case SseEventKind::ContentBlockDelta: {
            // The simdjson fast path above handles the common case;
            // fall back to the nlohmann path for anything it couldn't.
            auto delta = j.value("delta", json::object());
            auto type = delta.value("type", "");
            if (type == "text_delta") {
                ctx.sink(StreamTextDelta{delta.value("text", "")});
            } else if (type == "input_json_delta") {
                ctx.sink(StreamToolUseDelta{
                    ToolCallId{ctx.current_tool_id},
                    delta.value("partial_json", "")});
            } else if (type == "thinking_delta") {
                // Capture reasoning text for replay; also a liveness signal.
                ++ctx.thinking_deltas;
                ctx.sink(StreamThinkingDelta{delta.value("thinking", ""), {}});
            } else if (type == "signature_delta") {
                ++ctx.thinking_deltas;
                ctx.sink(StreamThinkingDelta{{}, delta.value("signature", "")});
            }
            break;
        }

        case SseEventKind::ContentBlockStop: {
            if (ctx.in_tool_use) {
                ctx.sink(StreamToolUseEnd{ToolCallId{ctx.current_tool_id}});
                ctx.in_tool_use = false;
                ctx.current_tool_id.clear();
                ctx.current_tool_name.clear();
            } else if (ctx.text_block_open) {
                // The prose block just closed — this ALWAYS precedes a
                // tool_use content_block_start, so it is the earliest
                // authoritative "model finished typing" signal. The view
                // drains the reveal cursor to the edge on it so the
                // mandatory hard-snap at the tool card is a no-op (no burst).
                ctx.text_block_open = false;
                ctx.sink(StreamTextBlockClosed{});
            }
            break;
        }

        case SseEventKind::MessageDelta: {
            if (j.contains("usage")) {
                const auto& u = j["usage"];
                StreamUsage su;
                su.input_tokens                = u.value("input_tokens", 0);
                su.output_tokens               = u.value("output_tokens", 0);
                su.cache_creation_input_tokens = u.value("cache_creation_input_tokens", 0);
                su.cache_read_input_tokens     = u.value("cache_read_input_tokens", 0);
                ctx.sink(su);
            }
            if (j.contains("delta") && j["delta"].contains("stop_reason")
                && j["delta"]["stop_reason"].is_string()) {
                ctx.stop_reason = parse_stop_reason(
                    j["delta"]["stop_reason"].get<std::string_view>());
            }
            break;
        }

        case SseEventKind::MessageStop: {
            if (ctx.in_tool_use) {
                ctx.sink(StreamToolUseEnd{ToolCallId{ctx.current_tool_id}});
                ctx.in_tool_use = false;
                ctx.current_tool_id.clear();
                ctx.current_tool_name.clear();
            }
            ctx.sink(StreamFinished{ctx.stop_reason});
            ctx.terminated = true;
            break;
        }

        case SseEventKind::Error: {
            // Mid-stream SSE error event. The wire payload is just
            // `error.type` + `error.message` — Anthropic doesn't surface
            // Retry-After here (it's an HTTP-header thing, and we already
            // passed the headers phase to enter the SSE body). Leave
            // retry_after unset and let the runtime fall back to its own
            // schedule.
            auto err = j.value("error", json::object());
            ctx.sink(StreamError{err.value("message", "unknown error"), std::nullopt});
            ctx.terminated = true;
            break;
        }
    }
}

} // namespace

void feed_sse(StreamCtx& ctx, const char* data, std::size_t len) {
    // Per-network-read boundary marker. The verbose `<< event=...` lines
    // alone can't tell a bursty wire (many text_deltas delivered in ONE
    // read after a gap) from a render-loop stall (deltas drip steadily but
    // the screen lags). This line stamps each read with its byte length so
    // the count of `<< event=` lines that follow before the NEXT `-- chunk`
    // is exactly the number of SSE events that arrived together.
    if (debug_log()) dbg("-- chunk len=%zu\n", len);
    // The shared framer owns the byte buffer, `event:`/`data:` accumulation,
    // the multi-line data join, the 4 MiB overflow cap, and amortized buffer
    // compaction. We only dispatch each complete event.
    ctx.sse.feed(data, len, [&](std::string_view name, std::string_view payload,
                                char* padded) {
        dispatch_event(ctx, name, payload, padded);
    });
}

} // namespace agentty::provider::anthropic
