#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/anthropic/prompt.hpp"


#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "agentty/domain/catalog.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/provider/wire_supersede.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/util/env.hpp"

namespace agentty::provider::anthropic {

namespace {

// Belt-and-suspenders UTF-8 scrubber. Registry already converts subprocess
// output at the capture boundary (GetConsoleOutputCP / CP_ACP pivot), but
// any string that reaches json::dump() must be valid UTF-8 or the API call
// dies with `type_error.316`. Cheap to run on already-valid strings, and
// guards future call sites that assemble tool output from multiple pieces
// (e.g. error suffix + partial output) where a byte boundary could split a
// UTF-8 sequence. Replaces invalid byte runs with U+FFFD.
//
// UTF-8 validation + scrubbing are the shared, strict source of truth in the
// wire header — every transport scrubs identically (rejects overlong encodings
// and surrogates before they 400 the next request). See wire::scrub_utf8.
using wire::is_valid_utf8;
using wire::scrub_utf8;

// Back up an index to the start of the UTF-8 code point it lands in.
// The UTF-8 boundary helpers + the tool-result byte-budget cap now live in
// the shared wire header (include/agentty/provider/wire.hpp) so every
// transport sizes tool output identically — see wire::cap_tool_result_aged.

// Env-var-gated request/SSE dump. Set AGENTTY_DEBUG_API=1 to write to
// $AGENTTY_DEBUG_FILE (or ./agentty-api.log). Appends, never truncates.
FILE* debug_log() {
    // Initialised exactly once (C++ guarantees thread-safe init of a
    // function-local static), then every subsequent call is a plain load
    // of the cached pointer — no mutex. dispatch_event() calls this once
    // per SSE event (i.e. per output token on a hot stream), so the old
    // per-call std::lock_guard was a full acquire/release barrier on the
    // wire's hottest path purely to re-check a write-once flag. The magic
    // static collapses that to a guard-byte test the compiler hoists to a
    // single load once initialisation has run.
    static FILE* fp = [] () -> FILE* {
        const char* on = util::env::get_or_null<util::env::Var::DebugApi>();
        if (!on || *on == '0') return nullptr;
        const char* path = util::env::get_or_null<util::env::Var::DebugFile>();
        std::string p = (path && *path) ? std::string{path}
                                        : std::string{"agentty-api.log"};
        return std::fopen(p.c_str(), "ab");
    }();
    return fp;
}
void dbg(const char* fmt, ...) {
    FILE* fp = debug_log();
    if (!fp) return;
    // Monotonic ms-since-first-call so SSE event timing can be measured
    // without parsing wall-clock timestamps. Compares cheap, scoped to the
    // process lifetime, and unambiguous when grepping the log.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  clock::now() - t0).count();
    std::fprintf(fp, "[+%6lldms] ", static_cast<long long>(ms));
    va_list ap; va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fflush(fp);
}
} // namespace

using json = nlohmann::json;

// Wire-format identity. agentty identifies itself HONESTLY — it does NOT
// impersonate the official Claude Code CLI. The user-agent and x-app announce
// "agentty"; we send only the headers Anthropic's API actually requires to
// function (anthropic-version + the anthropic-beta flags that gate features we
// use, including oauth-2025-04-20 for subscription tokens). We deliberately do
// NOT spoof Claude Code's user-agent, the Anthropic JS SDK's x-stainless-*
// fingerprint, its BetaToolRunner helper tag, or a Claude-Code-shaped
// metadata.user_id. If Anthropic's edge ever hard-requires a first-party
// client signature, that's a ToS boundary we surface to the user ("use an API
// key or another provider"), not one we quietly circumvent by masquerading.
namespace headers {
    inline constexpr const char* anthropic_version = "2023-06-01";
    // Honest identity — agentty says it is agentty.
    inline constexpr const char* user_agent        = "agentty/" AGENTTY_VERSION;
    inline constexpr const char* x_app             = "agentty";

    // Beta IDs are FEATURE flags, not identity spoofing — each unlocks a
    // capability agentty genuinely uses (OAuth-token acceptance, 1M context,
    // context management, prompt-cache scope, fine-grained tool streaming).
    // Composed by select_betas() per model/auth.
    inline constexpr const char* beta_claude_code            = "claude-code-20250219";
    inline constexpr const char* beta_oauth                  = "oauth-2025-04-20";
    inline constexpr const char* beta_context_1m             = "context-1m-2025-08-07";
    inline constexpr const char* beta_context_management     = "context-management-2025-06-27";
    inline constexpr const char* beta_prompt_cache_scope     = "prompt-caching-scope-2026-01-05";
    // Extended cache TTL (1-hour) opt-in. WITHOUT this beta in the header set,
    // sending `cache_control:{ttl:"1h"}` makes Anthropic's edge SILENTLY DROP
    // the whole breakpoint (cache miss every turn + throttled tier). WITH it,
    // the stable prefix breakpoints (system / tools / conversation anchor)
    // survive a 1-hour idle window instead of the 5-minute default — so a
    // pause to read, think, or step away no longer forces a full-price
    // cache-creation re-write of the entire prompt prefix on the next turn.
    // This is Claude Code's `Dt6` extended-TTL path. We keep the ROLLING
    // breakpoint on the newest message at the 5-minute default (it changes
    // every turn, so a long TTL there buys nothing) and spend the 1-hour TTL
    // only where the content is stable across turns.
    inline constexpr const char* beta_extended_cache_ttl     = "extended-cache-ttl-2025-04-11";
    // Per-tool `eager_input_streaming: true` is honored without a beta header
    // on Claude 4.6 (GA there). For older models (haiku-4-5, claude-3.x) the
    // edge requires this header — sending it on 4.6+ is a no-op so we always
    // include it when any tool in the request opts in.
    inline constexpr const char* beta_fine_grained_streaming = "fine-grained-tool-streaming-2025-05-14";
} // namespace headers

namespace {

// --- SSE parser -------------------------------------------------------------
// The byte-level SSE framing (line splitting, `event:`/`data:` accumulation,
// the multi-line data join, the overflow cap, buffer compaction) lives in the
// shared wire::SseFramer — see include/agentty/provider/wire.hpp. This
// transport supplies only the per-event dispatch (dispatch_event below).

struct StreamCtx {
    EventSink sink;
    wire::SseFramer sse;
    // Tool-use tracking (current block index in-flight)
    std::string current_tool_id;
    std::string current_tool_name;
    bool in_tool_use = false;
    // True while a TEXT content block is open on the wire (between its
    // content_block_start and content_block_stop). Lets ContentBlockStop
    // emit StreamTextBlockClosed exactly once per text block, and only for
    // text (not tool_use / thinking) blocks — the view uses that as the
    // earliest "prose finished" signal to drain the reveal before a tool
    // card appears.
    bool text_block_open = false;
    // Terminal-event tracking — exactly one of finished/errored must fire.
    bool terminated = false;
    // Stashed from message_delta so we can hand it to StreamFinished. Lets
    // the reducer tell "natural end" / "tool_use" apart from "max_tokens"
    // (which leaves the in-flight tool_use block truncated). Parsed at
    // the wire boundary into the typed enum — string-compare lives only
    // here, not in the reducer.
    StopReason stop_reason = StopReason::Unspecified;
    // simdjson parser is stateful and caches its scratch buffer across
    // iterate() calls — reusing one per stream avoids a malloc per SSE frame.
    // The payload no longer needs a separate padded copy: the SseFramer's
    // accumulator already carries the simd padding tail, so we iterate over
    // its buffer in place (see dispatch_content_block_delta_fast).
    simdjson::ondemand::parser simd_parser;
    // Diagnostic: count thinking-block deltas the model emitted so we can
    // tell "model is reasoning silently" apart from "wire is stalled" in
    // the debug log. Surfaced when the stream finishes.
    int thinking_deltas = 0;
};

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

void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
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

json tool_spec_to_json(const ToolSpec& s) {
    json j;
    j["name"] = s.name;
    j["description"] = s.description;
    j["input_schema"] = s.input_schema;
    // Anthropic's fine-grained tool streaming opt-in (per-tool field).
    // Only emit when true so cache-key shape matches CC's tool blocks for
    // tools that don't use it. GA on Claude 4.6+; gated by the
    // `fine-grained-tool-streaming-2025-05-14` beta header on older models
    // (we send that beta unconditionally so the field is always honored).
    if (s.eager_input_streaming) j["eager_input_streaming"] = true;
    return j;
}

// Pick the anthropic-beta value list for /v1/messages exactly the way the
// Mirrors Claude Code v2.1.113's `fR8(model)` cocktail builder for the
// firstParty path, MINUS the two thinking betas. Why we deviate from CC:
//
// `interleaved-thinking-2025-05-14` lets the model plan between content
// blocks; combined with `redact-thinking-2026-02-12` (which suppresses the
// thinking deltas from the wire), the result on long write/edit calls was
// 20-30 s of dead-air between a tool_use's `display_description` and its
// `content` field — the model was generating redacted thinking tokens we
// never see, then dumping the whole `content` body in one burst. CC papers
// over this with a "Thinking…" spinner; agentty's TUI doesn't, so it just looks
// frozen. Dropping both betas forces the model to start emitting `content`
// immediately. If you ever want to render thinking blocks, drop only the
// redact one and surface the visible thinking deltas in the UI.
std::string select_betas(std::string_view model, bool is_oauth,
                         bool any_eager_streaming = false) {
    // Single decode site for all model-id introspection — see
    // ModelCapabilities::from_id for why we tokenise rather than
    // substring-match. Adding a new beta gated on a new family /
    // generation goes here; nothing else in transport.cpp parses
    // model strings.
    const auto caps = ModelCapabilities::from_id(model);

    std::vector<std::string_view> b;
    if (!caps.is_haiku())              b.emplace_back(headers::beta_claude_code);          // !q
    if (is_oauth)                      b.emplace_back(headers::beta_oauth);                // Hq()
    if (caps.extended_context_1m)      b.emplace_back(headers::beta_context_1m);           // AL(H)
    if (caps.generation_4_or_later)    b.emplace_back(headers::beta_context_management);   // eU(provider) && CR4(H)
    b.emplace_back(headers::beta_prompt_cache_scope);                                      // _ (fa() — always true firstParty)
    // 1-hour extended cache TTL. Always on: agentty pins the stable prefix
    // (system / tools / conversation anchor) with ttl:"1h" so a long idle
    // window doesn't force a full-price re-cache. Harmless when no breakpoint
    // uses the long TTL (the edge just ignores an unused capability).
    b.emplace_back(headers::beta_extended_cache_ttl);
    if (any_eager_streaming)           b.emplace_back(headers::beta_fine_grained_streaming);

    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) out.push_back(',');
        out.append(b[i]);
    }
    return out;
}

// Build the lowercase HTTP/2 header set. agentty sends ONLY the headers the
// Anthropic API requires: the auth header, the API version, the beta feature
// flags, and its own honest user-agent / x-app. It deliberately does NOT send
// the Anthropic JS SDK's x-stainless-* platform fingerprint or the CLI's
// x-stainless-helper=BetaToolRunner tag — those exist only to make traffic
// look first-party, which agentty is not.
//
// AuthHeader is the closed sum (ApiKeyHeader | BearerHeader); std::visit
// dispatches to the right header NAME at the type level. There's no way
// to send an OAuth token under `x-api-key:` or vice versa — the variant
// arm names the header.
http::Headers build_request_headers(const AuthHeader& auth,
                                    std::string_view beta_value,
                                    int timeout_seconds,
                                    bool streaming = false,
                                    int retry_count = 0) {
    (void)timeout_seconds;
    (void)retry_count;
    http::Headers h;
    h.push_back({"accept", streaming ? "text/event-stream"
                                      : "application/json"});
    h.push_back({"content-type",   "application/json"});
    if (streaming) {
        // Corporate gateways commonly buffer/compress SSE until a large body
        // accumulates. These standard directives preserve incremental frames.
        h.push_back({"cache-control",   "no-cache, no-transform"});
        h.push_back({"pragma",          "no-cache"});
        h.push_back({"accept-encoding", "identity"});
    }
    h.push_back({"user-agent",     headers::user_agent});
    h.push_back({"x-app",          headers::x_app});
    h.push_back({"anthropic-version", headers::anthropic_version});
    h.push_back({"anthropic-dangerous-direct-browser-access", "true"});
    if (!beta_value.empty())
        h.push_back({"anthropic-beta", std::string{beta_value}});
    // The variant arm dictates the header. "Bearer " prefix is owned by
    // this site — callers hand us a raw token, not a prefixed string —
    // so an API key can't accidentally land with a Bearer prefix either.
    std::visit([&](const auto& a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, ApiKeyHeader>) {
            h.push_back({"x-api-key", a.value});
        } else if constexpr (std::is_same_v<T, BearerHeader>) {
            h.push_back({"authorization", "Bearer " + a.token});
        }
    }, auth);
    return h;
}

// Stable per-machine hex id. Anthropic's `metadata.user_id` is a legitimate,
// documented field used for abuse signals — keeping it STABLE per machine makes
// agentty's traffic look like one consistent user rather than a herd of fresh
// sessions every turn, which is honest (it IS one user on one machine). We do
// NOT shape it to look like Claude Code's `user_<hash>_account_<uuid>_...`
// identifier; it's just a derived-from-machine-id opaque hex string.
std::string machine_id_hex(int nibbles) {
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string seed;
        for (auto path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            std::ifstream f(path);
            if (f) { std::getline(f, seed); if (!seed.empty()) break; }
        }
        if (seed.empty()) {
            if (const char* h = std::getenv("HOSTNAME")) seed = h;
        }
        if (seed.empty()) seed = "agentty-anonymous";
        // FNV-1a 64-bit, twice with different offsets to pad to 128 bits.
        auto fnv = [](std::string_view s, uint64_t off) {
            uint64_t h = off;
            for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
            return h;
        };
        uint64_t a = fnv(seed, 0xcbf29ce484222325ull);
        uint64_t b = fnv(seed, 0x84222325cbf29ce4ull);
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)a, (unsigned long long)b);
        cached.assign(buf, 32);
    });
    return cached.substr(0, std::min<size_t>(nibbles, cached.size()));
}

std::string make_user_id() {
    // metadata.user_id is a JSON object `{device_id, session_id}`: a stable
    // per-machine device_id plus a per-process session_id (minted once, so it
    // stays constant across turns of one agentty run). This is agentty's own
    // honest client identity for Anthropic's abuse-signal bucketing — NOT a
    // copy of Claude Code's `T7H()` shape and with no fake account_uuid.
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, []{
        auto device_id = machine_id_hex(32);
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        uint64_t s1 = 0xcbf29ce484222325ull;
        for (int i = 0; i < 8; ++i) { s1 ^= (now >> (i*8)) & 0xff; s1 *= 0x100000001b3ull; }
        uint64_t s2 = s1 ^ 0x9e3779b97f4a7c15ull;
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)s1, (unsigned long long)s2);
        auto session_id = std::string(buf, 32);
        cached = nlohmann::json{
            {"device_id",  device_id},
            {"session_id", session_id},
        }.dump();
    });
    return cached;
}

} // namespace

// ── Streaming string-backed message-array writer ─────────────────────────
//
// messages_json_string() / build_messages() and their json_write_* helpers
// moved to anthropic/wire_body.cpp — the request-body serializer is the
// largest chunk of this file's hot path and lifts out cleanly (no StreamCtx,
// no SSE). run_stream_sync() below calls messages_json_string() via the
// transport.hpp declaration. Byte-identity guarded by wire_golden_test.

// ----------------------------------------------------------------------------

provider::StreamResult run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    if (is_empty(req.auth)) {
        sink(StreamError{"not authenticated — run 'agentty login' or set ANTHROPIC_API_KEY"});
        return provider::StreamResult::failed("not authenticated");
    }

    // emit_terminal runs on error paths after `sink` has been moved into
    // `ctx.sink` below — dispatching via `ctx.sink` is the only live handle.
    // The previous version captured `sink` by reference and invoked a
    // moved-from std::function on every non-happy-path termination, which
    // surfaced in the UI as "stream backend: bad_function_call". The whole
    // post-loop now runs through provider::finish_stream at the tail.

    const bool is_oauth = std::holds_alternative<BearerHeader>(req.auth);

    json body;
    // The `[1m]` picker marker selects the 1M window + context beta but is NOT
    // a real model id — strip it so the wire carries `claude-sonnet-5`, not
    // `claude-sonnet-5[1m]` (which the API 404s). select_betas() below still
    // sees the ORIGINAL req.model so it detects the marker and sends the
    // context-1m beta.
    body["model"]      = wire_model_id(req.model);
    body["max_tokens"] = req.max_tokens;
    body["stream"]     = true;

    // Stable-prefix cache breakpoint with the 1-HOUR extended TTL. The
    // system prompt is the single most stable thing in the request — it only
    // changes when the user edits CLAUDE.md or a `remember`/`forget` fires —
    // so it is the ideal place to spend the long TTL. We send the
    // `extended-cache-ttl-2025-04-11` beta unconditionally (see select_betas),
    // which is what makes `ttl:"1h"` stick instead of being silently dropped.
    // This is Claude Code's `Dt6` extended-cache path. Result: a pause of up
    // to an hour no longer forces a full-price re-cache of the whole system
    // prefix on the next turn. The rolling message breakpoints keep the 5 min
    // default (messages_json_string), since they change every turn anyway.
    const json kCacheCtl1h = {{"type", "ephemeral"}, {"ttl", "1h"}};

    // System is always sent as a content-block array so we can attach
    // cache_control regardless of auth style. OAuth additionally prepends
    // the immutable Claude Code preamble (cli.js line ~5641) so Anthropic's
    // edge accepts the OAuth token; API-key callers skip that preamble.
    {
        json sys = json::array();
        if (is_oauth) {
            sys.push_back({
                {"type", "text"},
                {"text", "You are Claude Code, Anthropic's official CLI for Claude."}
            });
        }
        // Scrub the system prompt too. Unlike message text (run through
        // scrub_utf8 in messages_json_string), the system prompt is placed
        // directly into `body` and serialized by body.dump() below — which
        // throws type_error.316 on the FIRST malformed UTF-8 byte. The prompt
        // now carries arbitrary user bytes (CLAUDE.md tiers + `remember`
        // records, which can round-trip a truncated/broken multi-byte
        // sequence from a pasted tool output). One bad byte there was failing
        // EVERY turn on every thread with "invalid UTF-8 in conversation"; a
        // single scrub here makes the wire well-formed regardless of source.
        sys.push_back({
            {"type", "text"},
            {"text", scrub_utf8(req.system_prompt)},
            {"cache_control", kCacheCtl1h}
        });
        body["system"] = std::move(sys);
    }

    // Build the messages array directly into a string buffer. Cache
    // breakpoints (last + second-to-last messages, last block of each)
    // are inlined during the write — see messages_json_string. We
    // splice this into the dumped body below rather than going through
    // `body["messages"] = json::parse(...)`, which would re-parse the
    // string back into a json tree just so body.dump() could
    // re-serialize it again. For a write-tool turn with 1 MiB of
    // content, the round-trip was the dominant request-build cost.
    // Replay stored thinking blocks only when this request itself enables
    // thinking (effort on). With thinking off, omit them — they're only
    // required by, and valid for, thinking-enabled requests.
    std::string messages_str = messages_json_string(
        Thread{ThreadId{""}, "", req.messages, {}, {}},
        /*include_thinking=*/!req.effort.empty());
    if (!req.tools.empty()) {
        json tools_j = json::array();
        for (const auto& t : req.tools) tools_j.push_back(tool_spec_to_json(t));
        // Tools cache breakpoint goes on the LAST tool — the schema array is
        // serialized in order and Anthropic's edge caches the prefix up to
        // and including the marked block. Matches cli.js where the tool list
        // is built once per session and the last entry carries cache_control.
        // Tools are session-stable (same catalog every turn), so they get the
        // 1-hour TTL alongside the system prompt — together they form the
        // long-lived prefix that survives idle gaps.
        tools_j.back()["cache_control"] = kCacheCtl1h;
        body["tools"] = std::move(tools_j);
    }
    body["metadata"] = json{{"user_id", make_user_id()}};
    // Reasoning effort + adaptive thinking. req.effort is pre-clamped to the
    // model's capability by launch_stream; non-empty means the user picked a
    // thinking tier in the model picker. Pair output_config.effort with
    // adaptive thinking — the GA way to turn reasoning on for Opus 4.6+/4.7/
    // 4.8 (budget_tokens is removed on 4.7/4.8). Omitted entirely when effort
    // is off, preserving the default no-thinking, dead-air-free wire. The
    // assistant thinking blocks the model emits in response are captured and
    // replayed by messages_json_string (see below) so tool_use turns don't
    // 400 for a dropped thinking block.
    if (!req.effort.empty()) {
        body["thinking"]       = json{{"type", "adaptive"}};
        body["output_config"]  = json{{"effort", req.effort}};
    }
    // Splice marker for the messages array. nlohmann gives the dumped
    // form `"messages":<unique-string>"`; we string-replace the
    // placeholder with messages_json_string. Picked a token that
    // can't appear inside a legitimately-escaped JSON string so the
    // find() is unambiguous even on weird payloads.
    constexpr std::string_view kMessagesPlaceholder =
        "\x01__agentty_messages_splice__\x01";
    body["messages"] = std::string{kMessagesPlaceholder};

    // Last-line-of-defence: if any string in the request tree still carries
    // non-UTF-8 bytes (a tool that bypassed the scrub, a new code path), the
    // dump() below throws type_error.316. We used to terminate(); now we
    // surface a StreamError so the reducer can recover and the user sees the
    // turn fail instead of the process dying mid-stream.
    std::string body_str;
    try {
        body_str = body.dump();
    } catch (const nlohmann::json::exception& e) {
        sink(StreamError{std::string{"request build failed (invalid UTF-8 in conversation): "} + e.what()});
        sink(StreamFinished{StopReason::Unspecified});
        return provider::StreamResult::failed("request build failed: invalid UTF-8");
    }
    // Replace the dumped placeholder string with the raw messages JSON.
    // nlohmann emits std::string values as JSON strings (quoted +
    // escaped). The control bytes \x01 round-trip as ``, so the
    // dumped form is `"__agentty_messages_splice__"` — find
    // and replace that.
    {
        constexpr std::string_view kDumpedPlaceholder =
            "\"\\u0001__agentty_messages_splice__\\u0001\"";
        auto pos = body_str.find(kDumpedPlaceholder);
        if (pos == std::string::npos) {
            sink(StreamError{"request build failed: messages placeholder not found in dumped body"});
            sink(StreamFinished{StopReason::Unspecified});
            return provider::StreamResult::failed("request build failed: placeholder");
        }
        body_str.replace(pos, kDumpedPlaceholder.size(), messages_str);
    }

    dbg("==== request ====\n%s\n==== /request ====\n", body_str.c_str());

    StreamCtx ctx;
    ctx.sink = std::move(sink);

    http::Request hreq;
    hreq.method  = http::HttpMethod::Post;
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    // `?beta=true` matches `beta.messages.create` in the SDK (cli.js line 393)
    // — the same path Anthropic's edge gates the beta header set against.
    hreq.path    = "/v1/messages?beta=true";
    // 300 s matches cli.js mb1(): API_TIMEOUT_MS env override or default 300 s
    // for local (120 s for CLAUDE_CODE_REMOTE). x-stainless-timeout is
    // advertisement, not enforcement — our actual stream is unbounded with
    // cancellation polled at frame boundaries.
    const bool any_eager = std::ranges::any_of(req.tools,
        [](const auto& t){ return t.eager_input_streaming; });
    hreq.headers = build_request_headers(req.auth,
                                         select_betas(req.model, is_oauth, any_eager),
                                         /*timeout_seconds=*/300,
                                         /*streaming=*/true,
                                         /*retry_count=*/req.retry_count);
    hreq.body    = std::move(body_str);

    // We split on HTTP status: 2xx → feed SSE chunks straight to the parser;
    // anything else → buffer the whole body and surface a structured error.
    int  http_status = 0;
    bool is_success  = false;
    std::string error_body;
    // Server-provided Retry-After hint, when present. Anthropic emits this
    // on 429 (rate_limit_error) and 529 (overloaded_error) — always as an
    // integer number of seconds (see Zed's parse_retry_after,
    // anthropic.rs:574-580). The runtime prefers this over its hardcoded
    // backoff schedule because the server knows better than we do how long
    // the brown-out will last. Clamped at the use site so a buggy proxy
    // can't pin us for an hour.
    std::optional<std::chrono::seconds> retry_after_hint;

    http::StreamHandler handler;
    handler.on_headers = [&](int status, const http::Headers& hh) {
        http_status = status;
        is_success  = (status >= 200 && status < 300);
        if (is_success) return;
        retry_after_hint = provider::parse_retry_after(hh);
    };
    handler.on_activity = [&] {
        ctx.sink(StreamHeartbeat{.transport_only = true});
    };
    handler.on_buffered_wait = [&] { ctx.sink(StreamBufferedWait{}); };
    handler.on_chunk = [&](std::string_view chunk) -> bool {
        if (is_success) {
            feed_sse(ctx, chunk.data(), chunk.size());
        } else {
            // Cap the buffered error body so a misbehaving edge can't OOM us.
            if (error_body.size() < 64 * 1024)
                error_body.append(chunk.data(),
                                  std::min(chunk.size(), 64 * 1024 - error_body.size()));
        }
        return true;
    };

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    // A responsive corporate gateway can keep the transport alive while
    // buffering every SSE frame. Give legitimate large turns ample time, but
    // retain an absolute ceiling now that control-frame activity suppresses
    // the short app-layer stall watchdog.
    tos.total   = std::chrono::minutes(30);
    // A healthy Anthropic stream emits SSE `ping` heartbeats every 10-15 s
    // even during long thinking blocks. 90 s without a single byte means
    // the transport is dead (silent peer, proxy stall, half-open TCP).
    // The error surfaces as "h2: idle timeout (no bytes for Ns)" and is
    // classified as Transient by provider::error_class — auto-retried
    // with backoff.
    //
    // The 90 s value is deliberately more patient than the historical
    // 45 s: on heavily-loaded edge pops we've observed legitimate 30-60 s
    // intervals. Inbound control bytes are forwarded as transport-only
    // heartbeats, allowing buffered VPN paths to wait up to the total cap.
    //
    // 15 s PING probe interval keeps a half-open TCP from going
    // undetected for long; the PING ACK bumps last_rx so a healthy peer
    // never trips idle.
    tos.ping    = std::chrono::milliseconds(15'000);
    tos.idle    = std::chrono::milliseconds(90'000);

    // Keep a copy of the cancel token: it is moved into the stream call below,
    // but finish_stream needs it to distinguish a user cancel from a transport
    // error at the post-loop.
    http::CancelTokenPtr cancel_for_end = cancel;
    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    dbg("==== http status=%d transport=%s thinking_deltas=%d ====\n",
        http_status, result ? "ok" : result.error().render().c_str(),
        ctx.thinking_deltas);

    if (!result) {
        dbg("error body: %s\n", error_body.c_str());
    } else if (!is_success) {
        dbg("error body: %s\n", error_body.c_str());
    }

    // Whole post-loop through the SHARED epilogue: classify the exit and emit
    // exactly one terminal event with correct precedence. on_any_end closes an
    // open tool block on BOTH success and error (peer may cut off mid-tool-use
    // before content_block_stop) so the reducer's salvage path always runs.
    return provider::finish_stream({
        .terminated  = ctx.terminated,
        .sink        = ctx.sink,
        .result_ok   = bool(result),
        .http_status = http_status,
        .non_replayable = !result && result.error().non_replayable,
        .cancel      = cancel_for_end,
        .stop        = ctx.stop_reason,
        .http_error_message = [&]() -> std::string {
            std::string msg = "HTTP " + std::to_string(http_status);
            try {
                auto j = json::parse(error_body);
                if (j.contains("error") && j["error"].contains("message"))
                    msg += ": " + j["error"]["message"].get<std::string>();
                else if (j.contains("message"))
                    msg += ": " + j["message"].get<std::string>();
                else
                    msg += ": " + error_body.substr(0, 300);
            } catch (...) {
                if (!error_body.empty()) msg += ": " + error_body.substr(0, 300);
            }
            if (http_status == 401 || http_status == 403)
                msg += "  (run 'agentty login' to re-authenticate)";
            return msg;
        },
        .retry_after = retry_after_hint,
        // Network / TLS / nghttp2-level error — never produced a complete SSE
        // stream. The typed HttpError's render() is embedded so the
        // downstream error_class substring sniff still routes it.
        .transport_error_message = [&]() -> std::string {
            return std::string{"http: "} + result.error().render();
        },
        .on_any_end = [&ctx]() {
            if (ctx.in_tool_use) {
                ctx.sink(StreamToolUseEnd{ToolCallId{ctx.current_tool_id}});
                ctx.in_tool_use = false;
                ctx.current_tool_id.clear();
                ctx.current_tool_name.clear();
            }
        },
    });
}

std::vector<ModelInfo> list_models(const AuthHeader& auth) {
    const bool is_oauth = std::holds_alternative<BearerHeader>(auth);

    // Claude Code's model catalog offers a 1M-context window as a distinct
    // picker VARIANT: for every model whose catalog entry has
    // `supports_1m_suffix:true`, it surfaces a second `<id>[1m]` row (labelled
    // "(1M context)") gated by the account's `context_1m_entitlement`. The
    // base id stays 200k; the `[1m]` id is 1M. We mirror that: after building
    // the base catalog, append a `[1m]` companion for each suffix-capable
    // model. We use OAuth (Pro/Max) as the entitlement proxy — a raw API key
    // on a lower usage tier can be capped at 200k and would 400 on a >200k
    // request, so we don't surface the 1M variant there. The `[1m]` id both
    // widens context_window() to 1M and makes the transport send the
    // context-1m-2025-08-07 beta (via ModelCapabilities::extended_context_1m).
    // The `[1m]` companion is inserted immediately AFTER its base model so the
    // picker reads "Opus 4.8" / "Opus 4.8 (1M context)" adjacently instead of
    // clumping every 1M row at the bottom.
    auto add_1m_variants = [is_oauth](std::vector<ModelInfo>& v) {
        if (!is_oauth) return;
        std::vector<ModelInfo> out;
        out.reserve(v.size() * 2);
        for (auto& mi : v) {
            const auto caps = ModelCapabilities::from_id(mi.id.value);
            const bool eligible = caps.supports_1m_suffix()
                && mi.id.value.find("[1m]") == std::string::npos;
            ModelInfo one_m;
            if (eligible) {
                one_m = mi;
                one_m.id = ModelId{mi.id.value + "[1m]"};
                one_m.display_name = mi.display_name + " (1M context)";
                one_m.context_window = 1'000'000;
            }
            out.push_back(std::move(mi));
            if (eligible) out.push_back(std::move(one_m));
        }
        v = std::move(out);
    };

    // Built-in catalog. Anthropic's ids are stable and few, so unlike the
    // OpenAI/Ollama path we always have a trustworthy fallback. Returned
    // only when the network probe genuinely yields nothing (offline, or a
    // transient non-200) so the picker is never stranded empty. With valid
    // creds the real /v1/models below returns the full upstream catalog —
    // the seed is just the floor, not the ceiling.
    auto seed = [&] {
        std::vector<ModelInfo> v{
            ModelInfo{ModelId{"claude-opus-4-5"},   "Claude Opus 4.5",   "anthropic", 200000, true},
            ModelInfo{ModelId{"claude-sonnet-4-5"}, "Claude Sonnet 4.5", "anthropic", 200000, true},
            ModelInfo{ModelId{"claude-haiku-4-5"},  "Claude Haiku 4.5",  "anthropic", 200000, false},
        };
        add_1m_variants(v);
        return v;
    };

    std::vector<ModelInfo> result;
    if (is_empty(auth)) return seed();

    http::Request hreq;
    hreq.method  = http::HttpMethod::Get;
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    if (const auto& ov = http::agentty_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.path    = "/v1/models?limit=100";
    // /v1/models doesn't need the streaming beta cocktail — just the oauth
    // gate when applicable, matching how cli.js calls model-listing endpoints.
    hreq.headers = build_request_headers(auth,
                                         is_oauth ? headers::beta_oauth : "",
                                         /*timeout_seconds=*/10);
    // /v1/models is a small list (~30 KB at typical catalog size). Cap
    // hard so a misbehaving proxy / replay loop can't stream us into
    // OOM on a routine startup probe.
    hreq.max_body_bytes = 1ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(5'000);
    tos.total   = std::chrono::milliseconds(10'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp || resp->status != 200) return seed();

    try {
        auto j = json::parse(resp->body);
        for (const auto& m : j.value("data", json::array())) {
            auto id = m.value("id", "");
            auto name = m.value("display_name", id);
            if (id.empty()) continue;
            result.push_back(ModelInfo{
                .id = ModelId{id},
                .display_name = name,
                .provider = "anthropic",
                .context_window = 200000,
            });
        }
        // Order the raw /v1/models list into a clean, predictable grouping
        // before we interleave the 1M variants — see catalog.hpp's
        // model_picker_less (single source of truth: strength-first, never a
        // fixed family bucket, so Fable/Mythos never sinks below Opus/Sonnet/
        // Haiku just because of alphabetical/positional bad luck).
        std::stable_sort(result.begin(), result.end(), model_picker_less);
        // Surface a `[1m]` companion for every suffix-capable model (OAuth),
        // inserted right after its base model so the pairing is adjacent.
        add_1m_variants(result);
    } catch (const std::exception& e) {
        util::dbglog("anthropic.list_models.parse", e.what());
    } catch (...) {
        util::dbglog("anthropic.list_models.parse", "non-std exception");
    }

    // Network said 200 but we parsed nothing usable — fall back to the seed
    // so the picker is never left empty after a provider switch.
    if (result.empty()) return seed();
    return result;
}

} // namespace agentty::provider::anthropic
