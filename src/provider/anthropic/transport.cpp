#include "agentty/provider/anthropic/transport.hpp"

#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/skills.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "agentty/domain/catalog.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/wire.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/util/base64.hpp"
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
// Fast path: validate in a single pass with no allocation. Almost all
// strings reaching this function are already valid UTF-8 (model output,
// user typed input, well-behaved tool stdout), so the slow rewriter only
// ever runs when there's actually something to fix. On a 100-message
// conversation this turns an O(N) per-byte rewrite into an O(N) validate
// — same big-O, but no per-byte std::string append and no allocation
// per message in the request build.
#if defined(__GNUC__) || defined(__clang__)
[[gnu::hot]]
#endif
inline bool is_valid_utf8(std::string_view in) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) { ++p; continue; }
        int extra; unsigned char mask; std::uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else return false;
        if (p + extra >= end) return false;
        std::uint32_t cp = c & mask;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = p[k];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += extra + 1;
    }
    return true;
}

std::string scrub_utf8(std::string_view in) {
    if (is_valid_utf8(in)) return std::string{in};

    std::string out;
    out.reserve(in.size());
    auto repl = [&]{ out.append("\xEF\xBF\xBD"); };
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); ++i; continue; }
        int extra; unsigned char mask; uint32_t min_cp;
        if      ((c & 0xE0) == 0xC0) { extra = 1; mask = 0x1F; min_cp = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; mask = 0x0F; min_cp = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; mask = 0x07; min_cp = 0x10000; }
        else { repl(); ++i; continue; }
        if (i + (size_t)extra >= in.size()) { repl(); ++i; continue; }
        uint32_t cp = c & mask;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = (unsigned char)in[i + (size_t)k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok || cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            repl(); ++i; continue;
        }
        out.append(in.data() + i, (size_t)(extra + 1));
        i += (size_t)extra + 1;
    }
    return out;
}

// Back up an index to the start of the UTF-8 code point it lands in.
// `i` is a prospective cut point; if it falls inside a multi-byte
// sequence we walk left until a lead byte (or 0). Bounded at 3
// continuation bytes (max UTF-8 sequence is 4 bytes).
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

// Wire-only byte-budget cap for a single tool result. A 500 KiB grep or
// a `read` of a giant file otherwise bloats EVERY subsequent request
// (the result replays on each turn) until auto-compaction eventually
// kicks in. Mirrors Zed's take_text_within_byte_budget (thread.rs):
// cap at a byte budget, cut on a UTF-8 boundary, splice a marker so the
// model knows bytes were elided. We keep head + tail (not just head)
// because tool output often carries the operative summary or error at
// the END (compiler tally, "N matches", exit status). Transcript is
// untouched -- the user still sees the full output; only the wire copy
// is trimmed.
[[nodiscard]] std::string cap_tool_result(std::string_view in, std::size_t budget) {
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

// Wire-format identity. We send byte-for-byte the same headers the official
// Claude Code CLI sends so OAuth tokens minted for "Claude Code" are accepted
// — Anthropic's edge gates oauth-2025-04-20 traffic on the matching x-app /
// user-agent / anthropic-beta combination. Pinned to the CLI version we
// reverse-engineered against (cli.js BUILD_TIME 2026-04-17, VERSION 2.1.113);
// refresh when a newer release adds beta flags we want to ride.
namespace headers {
    inline constexpr const char* anthropic_version = "2023-06-01";
    inline constexpr const char* claude_cli_version = "2.1.113";
    inline constexpr const char* anth_sdk_version  = "0.81.0";
    inline constexpr const char* node_runtime_ver  = "v22.11.0";
    // Matches CC's `bA()`: literal `claude-code/<VERSION>` — no "(external,
    // cli)" suffix, no `claude-cli` prefix. Older suffix variant correlated
    // with mid-stream buffering on long tool_use bodies.
    inline constexpr const char* user_agent        = "claude-code/2.1.113";
    inline constexpr const char* x_app             = "cli";

    // Beta IDs (literal strings extracted from the v2.1.113 binary's
    // `fR8(model)` builder). Listed individually so select_betas() can compose
    // the call-site set in the same order Claude Code's cocktail-builder
    // pushes them. Thinking betas (interleaved-thinking, redact-thinking) are
    // intentionally absent — see select_betas() for why.
    inline constexpr const char* beta_claude_code            = "claude-code-20250219";
    inline constexpr const char* beta_oauth                  = "oauth-2025-04-20";
    inline constexpr const char* beta_context_1m             = "context-1m-2025-08-07";
    inline constexpr const char* beta_context_management     = "context-management-2025-06-27";
    inline constexpr const char* beta_prompt_cache_scope     = "prompt-caching-scope-2026-01-05";
    // Per-tool `eager_input_streaming: true` is honored without a beta header
    // on Claude 4.6 (GA there). For older models (haiku-4-5, claude-3.x) the
    // edge requires this header — sending it on 4.6+ is a no-op so we always
    // include it when any tool in the request opts in. Discovered from CC's
    // breaking-changes doc strings ("fine-grained-tool-streaming-2025-05-14 →
    // GA on 4.6, remove") and verified against zed-industries/zed's
    // crates/anthropic completion path.
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
    simdjson::ondemand::parser simd_parser;
    simdjson::padded_string     simd_scratch;
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
bool dispatch_content_block_delta_fast(StreamCtx& ctx, std::string_view data) {
    const std::size_t need = data.size() + simdjson::SIMDJSON_PADDING;
    if (ctx.simd_scratch.size() < need) {
        ctx.simd_scratch = simdjson::padded_string(need);
    }
    std::memcpy(ctx.simd_scratch.data(), data.data(), data.size());
    // simdjson only requires the padding bytes be *readable*, not zeroed
    // — padded_string's ctor already zero-fills on allocation, and we
    // only ever read past `data.size()` from within simdjson's SIMD
    // loads, which tolerate junk. Skipping the per-frame memset saves
    // ~64 B of writes on every content_block_delta (~95% of stream
    // volume) for no observable behavior change.

    simdjson::ondemand::document doc;
    if (ctx.simd_parser.iterate(ctx.simd_scratch.data(), data.size(), need).get(doc))
        return false;

    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return false;

    simdjson::ondemand::object delta;
    if (root["delta"].get_object().get(delta)) return false;

    std::string_view delta_type;
    if (delta["type"].get_string().get(delta_type)) return false;

    if (delta_type == "text_delta") {
        std::string_view text;
        if (delta["text"].get_string().get(text)) return false;
        ctx.sink(StreamTextDelta{std::string{text}});
        return true;
    }
    if (delta_type == "input_json_delta") {
        std::string_view partial;
        if (delta["partial_json"].get_string().get(partial)) return false;
        ctx.sink(StreamToolUseDelta{std::string{partial}});
        return true;
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
        if (delta["thinking"].get_string().get(text)) return false;
        ++ctx.thinking_deltas;
        ctx.sink(StreamThinkingDelta{std::string{text}, {}});
        return true;
    }
    if (delta_type == "signature_delta") {
        std::string_view sig;
        if (delta["signature"].get_string().get(sig)) return false;
        ++ctx.thinking_deltas;
        ctx.sink(StreamThinkingDelta{{}, std::string{sig}});
        return true;
    }
    return false;
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

void dispatch_event(StreamCtx& ctx, std::string_view name, std::string_view data) {
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
    // closed-sum kind_of_event() lookup is fast enough we could route
    // it through the switch below, but the simdjson fast-path needs
    // the data buffer string intact, so we branch out here.
    if (name == "content_block_delta"
        && dispatch_content_block_delta_fast(ctx, data)) {
        return;
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
                ctx.sink(StreamToolUseDelta{delta.value("partial_json", "")});
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
                ctx.sink(StreamToolUseEnd{});
                ctx.in_tool_use = false;
                ctx.current_tool_id.clear();
                ctx.current_tool_name.clear();
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
                ctx.sink(StreamToolUseEnd{});
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
    ctx.sse.feed(data, len, [&](std::string_view name, std::string_view payload) {
        dispatch_event(ctx, name, payload);
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

// x-stainless-os literal. Mirrors the SDK's normalize-platform table in
// anth-sdk/internal/detect-platform.mjs lines 50-58.
constexpr const char* stainless_os() {
#if defined(__APPLE__)
    return "MacOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#elif defined(__OpenBSD__)
    return "OpenBSD";
#else
    return "Other";
#endif
}

constexpr const char* stainless_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x32";
#else
    return "unknown";
#endif
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
    if (any_eager_streaming)           b.emplace_back(headers::beta_fine_grained_streaming);

    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i) out.push_back(',');
        out.append(b[i]);
    }
    return out;
}

// Build the lowercase HTTP/2 header set in the same order Claude Code lays
// them out. Order isn't semantically required (HTTP doesn't care) but
// matching it makes wireshark dumps line up cleanly during debugging.
//
// `streaming=true` adds the same x-stainless-helper-method+x-stainless-helper
// pair that cli.js's MessageStream._createMessage() injects when entering the
// BetaToolRunner agent loop — Anthropic's edge keys some quotas off these.
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
    http::Headers h;
    h.push_back({"accept",         "application/json"});
    h.push_back({"content-type",   "application/json"});
    h.push_back({"user-agent",     headers::user_agent});
    h.push_back({"x-app",          headers::x_app});
    h.push_back({"anthropic-version", headers::anthropic_version});
    h.push_back({"anthropic-dangerous-direct-browser-access", "true"});
    if (!beta_value.empty())
        h.push_back({"anthropic-beta", std::string{beta_value}});
    h.push_back({"x-stainless-lang",            "js"});
    h.push_back({"x-stainless-package-version", headers::anth_sdk_version});
    h.push_back({"x-stainless-os",              stainless_os()});
    h.push_back({"x-stainless-arch",            stainless_arch()});
    h.push_back({"x-stainless-runtime",         "node"});
    h.push_back({"x-stainless-runtime-version", headers::node_runtime_ver});
    h.push_back({"x-stainless-retry-count",     std::to_string(retry_count < 0 ? 0 : retry_count)});
    h.push_back({"x-stainless-timeout",         std::to_string(timeout_seconds)});
    if (streaming) {
        // .stream() helpers in cli.js always set helper-method=stream; the
        // sibling x-stainless-helper carries the agent-loop tag so Anthropic
        // can distinguish raw API consumers from the official tool runner.
        h.push_back({"x-stainless-helper-method", "stream"});
        h.push_back({"x-stainless-helper",        "BetaToolRunner"});
    }
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

// Synthesize a Claude-Code-shaped metadata.user_id. The CLI uses
// `user_<emailHash>_account_<accountUuid>_session_<sessionUuid>`. We don't
// own an Anthropic account UUID, so we derive a stable per-machine hex
// triplet. Anthropic uses this for abuse signals — keeping it stable per
// machine makes our traffic look like a single legit user instead of a
// thundering herd of fresh sessions every turn.
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
    // CC v2.1.113's `T7H()` returns `metadata.user_id` as a JSON-stringified
    // object: `{"device_id":..,"account_uuid":..,"session_id":..}`. Earlier
    // CLI builds used the flat `user_<hex>_account_<hex>_session_<hex>` shape
    // — agentty shipped that and it correlated with a 20-30 s mid-stream pause
    // on long tool_use bodies. Anthropic's edge appears to inspect this field
    // for routing/quota; matching the new shape byte-for-byte is part of the
    // fix. We don't own a real account UUID under OAuth here, so we leave it
    // empty exactly as CC does when one isn't available.
    //
    // Cached for the agentty process lifetime so `session_id` stays stable
    // across turns — CC mints session_id once per CLI invocation. A fresh
    // session_id per request defeats the abuse-signal stability that the
    // stable device_id is meant to provide.
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
            {"device_id",    device_id},
            {"account_uuid", ""},
            {"session_id",   session_id},
        }.dump();
    });
    return cached;
}

} // namespace

// ── Streaming string-backed message-array writer ─────────────────────────
//
// Building the messages array via nlohmann::json was the largest hidden
// allocation in the request hot path. The `{"input", tc.args.is_object()
// ? tc.args : json::object()}` initializer-list deep-copies tc.args; for
// a `write` tool whose `content` field is a 1 MiB file body, that's a
// 1 MiB recursive json clone followed by another 1+ MiB allocation when
// `body.dump()` re-serializes it. Two big copies per request, paid again
// on every retry.
//
// `messages_json_string` writes the messages array directly into a
// std::string buffer, JSON-escaping inline. The win lands on tc.args:
// tc.args_dump() already caches the serialized form (used by the view
// for permission cards), so we splice those bytes verbatim into the
// "input" field. No clone, no re-parse, no re-dump. For unrecoverable
// edge cases (an args object that somehow lost its dump cache) we fall
// back to an on-demand dump rather than copying through json.
//
// Cache-breakpoint pinning (the `pin_last_block` helper that mutates
// the last content block of the last + second-to-last messages) is now
// done inline during write — we count messages first, then know in
// advance which are the pin-eligible ones.
namespace {

// True whenever an assistant message carries ANY tool_calls. Anthropic
// requires that every `tool_use` block be followed by a matching
// `tool_result` in the next message — sending the tool_use without its
// pair returns HTTP 400 ("`tool_use` ids were found without
// `tool_result` blocks immediately after") and, because the broken
// transcript is replayed on every subsequent turn, the session
// becomes wedged. We therefore emit the follow-up user turn whenever
// there's a tool_use to pair, even if some of them are still in a
// non-terminal state (Pending / Approved / Running). The non-terminal
// branches get a synthesized placeholder result downstream so the wire
// stays valid; the in-memory ToolUse status is left untouched.
[[nodiscard]] inline bool is_assistant_with_results(const Message& m) noexcept {
    return m.role == Role::Assistant && !m.tool_calls.empty();
}

// True iff the message carries at least one image with non-empty bytes.
// An empty-bytes ImageContent (e.g. a draft attachment whose body was
// already drained, leaked into a thread it doesn't belong to) must NOT
// drive the message-emission decision: serializing it produces an empty
// base64 "data" field that 400s the whole request.
[[nodiscard]] inline bool has_wire_image(const Message& m) noexcept {
    for (const auto& img : m.images)
        if (!img.bytes.empty()) return true;
    return false;
}

void json_write_escaped_string(std::string& out, std::string_view s) {
    out.push_back('"');
    out.reserve(out.size() + s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out.append("\\\"", 2); break;
            case '\\': out.append("\\\\", 2); break;
            case '\b': out.append("\\b",  2); break;
            case '\f': out.append("\\f",  2); break;
            case '\n': out.append("\\n",  2); break;
            case '\r': out.append("\\r",  2); break;
            case '\t': out.append("\\t",  2); break;
            default:
                if (c < 0x20) {
                    // \u00XX for control bytes.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.append(buf, 6);
                } else {
                    // Printable + UTF-8 multibyte: passthrough. We
                    // assume the caller already scrub_utf8'd inputs
                    // (text bodies, tool outputs, args), so multi-byte
                    // sequences here are well-formed.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void json_write_field(std::string& out, std::string_view key,
                      std::string_view value, bool& first) {
    if (!first) out.push_back(',');
    first = false;
    json_write_escaped_string(out, key);
    out.push_back(':');
    json_write_escaped_string(out, value);
}

// Splice raw pre-serialized JSON into a value slot (no escaping).
void json_write_raw_field(std::string& out, std::string_view key,
                          std::string_view raw_value, bool& first) {
    if (!first) out.push_back(',');
    first = false;
    json_write_escaped_string(out, key);
    out.push_back(':');
    out.append(raw_value);
}

void json_write_bool_field(std::string& out, std::string_view key,
                           bool v, bool& first) {
    json_write_raw_field(out, key, v ? "true" : "false", first);
}

// Cache-control marker for prompt caching. Compile-time string so we
// don't pay re-serialization on every breakpoint.
constexpr std::string_view kCacheCtlJsonRaw = R"({"type":"ephemeral"})";

void write_text_block(std::string& out, std::string_view text, bool pin_cache) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "text", first);
    json_write_field(out, "text", text, first);
    if (pin_cache) json_write_raw_field(out, "cache_control", kCacheCtlJsonRaw, first);
    out.push_back('}');
}

// Anthropic image content block:
//   {"type":"image","source":{"type":"base64",
//                              "media_type":"image/png","data":"..."}}
// `data` is standard RFC-4648 base64 (NOT base64url). We encode the
// bytes once at write time — keeping them raw in `ImageContent.bytes`
// avoids the +33% memory overhead in the running model state.
void write_image_block(std::string& out, const ImageContent& img,
                       bool pin_cache) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "image", first);
    if (!first) out.push_back(',');
    first = false;
    out.append(R"("source":{"type":"base64",)");
    out.append(R"("media_type":)");
    json_write_escaped_string(out,
        img.media_type.empty() ? std::string_view{"image/png"}
                               : std::string_view{img.media_type});
    out.append(R"(,"data":)");
    json_write_escaped_string(out, util::base64_encode(img.bytes));
    out.push_back('}');
    if (pin_cache) json_write_raw_field(out, "cache_control", kCacheCtlJsonRaw, first);
    out.push_back('}');
}

void write_tool_use_block(std::string& out, const ToolUse& tc, bool pin_cache) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "tool_use", first);
    json_write_field(out, "id",   tc.id.value, first);
    json_write_field(out, "name", tc.name.value, first);
    // Splice the cached args dump verbatim. args_dump() guarantees a
    // valid JSON-object string ("{}" minimum, never empty); fall back
    // to a fresh dump if for any reason the cache is in an unexpected
    // shape (defensive — shouldn't fire in practice).
    std::string_view dump = tc.args_dump();
    std::string fallback;
    if (dump.empty() || dump.front() != '{') {
        fallback = tc.args.is_object() ? tc.args.dump() : std::string{"{}"};
        dump = fallback;
    }
    json_write_raw_field(out, "input", dump, first);
    if (pin_cache) json_write_raw_field(out, "cache_control", kCacheCtlJsonRaw, first);
    out.push_back('}');
}

void write_tool_result_block(std::string& out, const ToolUse& tc, bool pin_cache) {
    out.push_back('{');
    bool first = true;
    json_write_field(out, "type", "tool_result", first);
    json_write_field(out, "tool_use_id", tc.id.value, first);
    // Non-terminal tools (Pending / Approved / Running) carry no
    // output yet. We still MUST emit a tool_result for them — see
    // is_assistant_with_results above for the wire-shape rationale —
    // so synthesize an `is_error: true` placeholder. Marking it as an
    // error tells the model the call didn't actually produce a result,
    // which is the truthful read of "the previous turn died before
    // this tool finished." Empty Done output stays as the historical
    // "(no output)" placeholder (not an error) for tools that
    // legitimately produced nothing.
    auto raw_output = tc.output();
    std::string scrubbed;
    const bool non_terminal = !tc.is_terminal();
    if (non_terminal) {
        json_write_field(out, "content",
            "(tool call did not complete \u2014 previous turn ended before this tool produced a result)",
            first);
    } else if (raw_output.empty()) {
        json_write_field(out, "content", "(no output)", first);
    } else {
        // Per-result wire cap. 64 KiB comfortably holds a full 2000-line
        // read or a large grep page while preventing one pathological
        // result (500 KiB grep, dump of a binary) from replaying on every
        // subsequent turn. Independent of compaction -- this fires per
        // request, immediately, with no transcript mutation.
        constexpr std::size_t kToolResultWireBudget = 64u * 1024u;
        std::string capped = cap_tool_result(raw_output, kToolResultWireBudget);
        scrubbed = scrub_utf8(capped);
        json_write_field(out, "content", scrubbed, first);
    }
    const bool is_error = non_terminal || tc.is_failed() || tc.is_rejected();
    json_write_bool_field(out, "is_error", is_error, first);
    if (pin_cache) json_write_raw_field(out, "cache_control", kCacheCtlJsonRaw, first);
    out.push_back('}');
}

} // namespace

[[nodiscard]] std::string messages_json_string(const Thread& t,
                                               bool include_thinking) {
    // First pass: figure out where the cache breakpoints land. cli.js
    // pins BOTH the last and second-to-last *emitted* messages' last
    // content blocks (rolling cache reuse — turn N's last becomes turn
    // N+1's second-to-last). A "message" here is whatever lands in the
    // output array, so an Assistant turn with terminal tool_calls
    // contributes TWO messages (assistant + tool_results follow-up).
    int total_msgs = 0;
    for (const auto& m : t.messages) {
        const bool has_images = (m.role == Role::User && has_wire_image(m));
        if (!m.text.empty()
         || has_images
         || (m.role == Role::Assistant && !m.tool_calls.empty())) {
            ++total_msgs;
        }
        if (is_assistant_with_results(m)) ++total_msgs;
    }
    const int pin_last       = total_msgs - 1;
    const int pin_second_last = total_msgs - 2;

    std::string out;
    // Conservative reserve: typical sessions are ~64 KiB; a write turn
    // can push past 1 MiB. Either way, let the std::string growth
    // strategy take it from here without an early reallocation.
    out.reserve(64 * 1024);
    out.push_back('[');

    int emitted = 0;
    auto emit_msg_open = [&] {
        if (emitted > 0) out.push_back(',');
        ++emitted;
    };
    auto pinning_for = [&](int idx) {
        return idx == pin_last || idx == pin_second_last;
    };

    for (const auto& m : t.messages) {
        // ── Primary message (text + tool_use blocks if Assistant) ──
        const bool has_text   = !m.text.empty();
        const bool has_images = (m.role == Role::User && has_wire_image(m));
        const bool has_tools  = (m.role == Role::Assistant && !m.tool_calls.empty());
        // Replay a captured thinking block on assistant turns that also
        // carry real content (text or tool_use). Anthropic requires the
        // block be present and verbatim on the turn whose tool_use it
        // precedes, or the request 400s. Gated on a present signature (an
        // unsigned thinking block is rejected) and on the request enabling
        // thinking (include_thinking).
        const bool has_thinking = include_thinking
                               && m.role == Role::Assistant
                               && !m.thinking_signature.empty()
                               && (has_text || has_tools);
        if (has_text || has_images || has_tools) {
            const int my_idx   = emitted;
            const bool do_pin  = pinning_for(my_idx);
            emit_msg_open();
            out.push_back('{');
            out.append(R"("role":)");
            out.append(m.role == Role::User ? R"("user")" : R"("assistant")");
            out.append(R"(,"content":[)");
            // Anthropic accepts mixed content arrays. Emit images
            // FIRST so the prose that references them ("describe this
            // screenshot") follows in the same content array — the
            // model reads in array order and benefits from having the
            // visual context loaded before the prompt text. Then the
            // text block, then any tool_use blocks (Assistant turns).
            // EMPTY-bytes images are skipped entirely: a stray
            // empty ImageContent (e.g. a draft attachment whose bytes
            // were already drained) would serialize an empty base64
            // "data" field and 400 the whole request.
            int wire_images = 0;
            if (has_images)
                for (const auto& img : m.images)
                    if (!img.bytes.empty()) ++wire_images;
            int blocks = (has_thinking ? 1 : 0)
                       + wire_images
                       + (has_text ? 1 : 0)
                       + (has_tools ? static_cast<int>(m.tool_calls.size()) : 0);
            int block_emitted = 0;
            // Thinking block goes FIRST — the model emits it before its
            // text/tool_use, and the replay order must match. It is never
            // the cache pin (content always follows it). json(...).dump()
            // JSON-encodes the (possibly empty) thinking text + opaque
            // signature; no cache_control on a thinking block.
            if (has_thinking) {
                if (block_emitted++ > 0) out.push_back(',');
                out.append(R"({"type":"thinking","thinking":)");
                out.append(json(scrub_utf8(m.thinking)).dump());
                out.append(R"(,"signature":)");
                out.append(json(m.thinking_signature).dump());
                out.push_back('}');
            }
            if (has_images) {
                for (const auto& img : m.images) {
                    if (img.bytes.empty()) continue;
                    if (block_emitted++ > 0) out.push_back(',');
                    const bool last_block = (block_emitted == blocks);
                    write_image_block(out, img, do_pin && last_block);
                }
            }
            if (has_text) {
                if (block_emitted++ > 0) out.push_back(',');
                const bool last_block = (block_emitted == blocks);
                // Expand chip placeholders (\x01ATT:N\x01) into
                // their attachment bodies so the model sees the
                // literal pasted text / file contents. The
                // transcript renderer keeps the chip form for the
                // user; only the wire payload sees the full bytes.
                // No-op when m.attachments is empty (no expansion
                // needed and no allocation either).
                std::string wire_text = m.attachments.empty()
                    ? m.text
                    : attachment::expand(m.text, m.attachments);
                write_text_block(out, scrub_utf8(wire_text), do_pin && last_block);
            }
            if (has_tools) {
                for (const auto& tc : m.tool_calls) {
                    if (block_emitted++ > 0) out.push_back(',');
                    const bool last_block = (block_emitted == blocks);
                    write_tool_use_block(out, tc, do_pin && last_block);
                }
            }
            out.append("]}");
        }

        // ── Tool-result follow-up (synthetic User turn) ──
        // Emit one tool_result per tool_use, terminal or not. The
        // wire shape Anthropic enforces is pairwise (every tool_use
        // id must appear as a tool_use_id in the next message), so
        // we cannot selectively drop the non-terminal ones — that's
        // exactly what triggered the HTTP 400 loop.
        if (is_assistant_with_results(m)) {
            const int my_idx   = emitted;
            const bool do_pin  = pinning_for(my_idx);
            emit_msg_open();
            out.append(R"({"role":"user","content":[)");
            const int total_results = static_cast<int>(m.tool_calls.size());
            int result_emitted = 0;
            for (const auto& tc : m.tool_calls) {
                if (result_emitted++ > 0) out.push_back(',');
                const bool last_block = (result_emitted == total_results);
                write_tool_result_block(out, tc, do_pin && last_block);
            }
            out.append("]}");
        }
    }

    out.push_back(']');
    return out;
}

// Compatibility shim: the public signature still returns json (callers
// outside transport.cpp's hot path may depend on it). The hot path uses
// `messages_json_string` directly.
json build_messages(const Thread& t) {
    return json::parse(messages_json_string(t, /*include_thinking=*/false));
}

namespace {

// Read a text file, swallowing any I/O error — returns empty string on
// missing file or unreadable. Capped at 64 KiB to keep one rogue
// 200 MB CLAUDE.md from poisoning the system prompt on every turn.
[[nodiscard]] std::string read_optional_memory(const std::filesystem::path& p) noexcept {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec) return {};
    auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz == 0 || sz > 64u * 1024u) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    out.resize(static_cast<std::size_t>(f.gcount()));
    // Trim trailing whitespace so the wrapper tag doesn't get a blank
    // line jammed against `</user-memory>`.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'
                            || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

// mtime-keyed cache wrapper around read_optional_memory. The CLAUDE.md
// hierarchy is read on every turn (3 files: ~/CLAUDE.md, ./CLAUDE.md,
// ./CLAUDE.local.md), each capped at 64 KiB. Per-turn that's:
//
//   • on a local SSD: ~3 × stat+open+read = sub-millisecond
//   • on an NFS home: ~3 × roundtrip ≈ 30-100 ms before the worker
//     even starts building the request
//
// The contents change between turns at most rarely (the user editing
// their CLAUDE.md mid-session). Cache by filesystem mtime: stat once
// to get last_write_time; if it matches the cached entry, hand back
// the cached body. Process-lifetime cache; bounded by the 3 paths
// collect_memory_blocks looks at and the 64 KiB-per-file content cap,
// so worst case ~192 KiB of process memory.
//
// Concurrency: the system prompt is built on the stream worker thread
// (Cmd::stream's task body). Two concurrent stream calls against the
// same Anthropic endpoint are serialized at the connection-pool layer,
// so there's effectively one writer in practice — but the mutex
// removes "in practice" from the contract.
[[nodiscard]] std::string read_memory_cached(const std::filesystem::path& p) {
    struct Entry {
        std::filesystem::file_time_type mtime{};
        std::string                     content;
    };
    static std::unordered_map<std::string, Entry> cache;
    static std::mutex                              mu;

    const std::string key = p.string();
    std::error_code   ec;
    auto              now_mtime = std::filesystem::last_write_time(p, ec);

    if (ec) {
        // File missing / unreadable — drop any previous cache entry so
        // a re-creation later is observed (the next call will re-stat
        // and miss into the read path).
        std::lock_guard lk(mu);
        cache.erase(key);
        return {};
    }

    {
        std::lock_guard lk(mu);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.mtime == now_mtime) {
            return it->second.content;
        }
    }

    // Cache miss or stale — read outside the lock so a slow NFS read
    // doesn't block other paths' cache hits.
    std::string body = read_optional_memory(p);
    {
        std::lock_guard lk(mu);
        cache[key] = Entry{now_mtime, body};
    }
    return body;
}

// Resolve the user's home directory portably. Tries HOME (POSIX) first,
// USERPROFILE (Windows) second; returns empty path on neither set.
[[nodiscard]] std::filesystem::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return std::filesystem::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return std::filesystem::path{h};
#endif
    return {};
}

// CLAUDE.md memory hierarchy — mirrors Claude Code's resolution
// (binary near offset 134900):
//
//   User    ~/CLAUDE.md             personal, all projects
//   Project <cwd>/CLAUDE.md         committed, project-specific
//   Local   <cwd>/CLAUDE.local.md   gitignored, personal-to-this-project
//
// Each tier is wrapped in its own tag so the model can tell them apart.
// Empty / missing tiers are silently elided. The wire cost is paid once
// per ~5 min cache_control TTL window regardless (the system-prompt
// cache_control breakpoint catches the result); the disk cost is
// memoized through read_memory_cached so the per-turn footprint is
// 3× stat() + memcpy of the cached body, not 3× full read.
[[nodiscard]] std::string collect_memory_blocks() {
    std::string user    = read_memory_cached(home_dir() / "CLAUDE.md");
    std::string project = read_memory_cached(std::filesystem::path{"CLAUDE.md"});
    std::string local   = read_memory_cached(std::filesystem::path{"CLAUDE.local.md"});

    // Agent-authored memory (written by the `remember` tool, removed by
    // `forget`). Loaded as tail-N from the per-scope JSONL stores so the
    // prompt stays bounded even if the on-disk files grow.
    auto learned_user    = tools::memory::load_recent_user();
    auto learned_project = tools::memory::load_recent_project();

    if (user.empty() && project.empty() && local.empty()
        && learned_user.empty() && learned_project.empty()) return {};

    std::ostringstream m;
    m << "\n\n<memory>\n"
      << "Project-specific guidance the user has authored. Treat these "
         "as persistent context for THIS workspace and user; lower tiers "
         "(local, then project, then user) win on conflicting rules.\n";
    if (!user.empty())    m << "<user-memory>\n"    << user    << "\n</user-memory>\n";
    if (!project.empty()) m << "<project-memory>\n" << project << "\n</project-memory>\n";
    if (!local.empty())   m << "<local-memory>\n"   << local   << "\n</local-memory>\n";
    auto emit_learned = [&](const char* tag, std::vector<tools::memory::Record> rs) {
        if (rs.empty()) return;
        // Budget the block so a large memory store can't inflate every
        // system prompt. select_for_prompt keeps all pinned records +
        // the highest-signal remainder within kPromptByteBudget, clipping
        // any over-long record; the rest stay on disk (recallable, still
        // editable) and we note the elided count so the model knows the
        // store holds more than what's shown.
        auto picked = tools::memory::select_for_prompt(std::move(rs));
        if (picked.records.empty() && picked.dropped == 0) return;
        m << "<learned-memory scope=\"" << tag << "\">\n"
          << "Facts you previously stored via the `remember` tool. Each "
             "line is prefixed with the record id — pass that id to "
             "`forget` if the fact is no longer true.\n";
        for (const auto& r : picked.records)
            m << tools::memory::render_for_prompt(r) << "\n";
        if (picked.dropped > 0)
            m << "[+" << picked.dropped << " more stored fact(s) not shown "
                 "here to keep the prompt small — they remain on disk; ask "
                 "about a topic and recall surfaces them, or pin the ones "
                 "that should always be visible.]\n";
        m << "</learned-memory>\n";
    };
    emit_learned("user",    std::move(learned_user));
    emit_learned("project", std::move(learned_project));
    m << "</memory>";
    return m.str();
}

} // namespace

std::string default_system_prompt() {
#if defined(_WIN32)
    constexpr const char* os_name  = "Windows";
    constexpr const char* shell    = "cmd.exe (Windows Command Prompt)";
    constexpr const char* shell_hint =
        "Prefer native Windows equivalents: `dir` / `where` / `systeminfo` / "
        "`type` / `findstr` / `powershell -c`. Do NOT use POSIX-only tools "
        "like `uname`, `cat /etc/os-release`, `sw_vers`, `ls`, `grep`, `sed`, "
        "`awk`, or shell heredocs (`<<EOF`) — they will fail. "
        "Commands chain with `&&` and `||` under cmd.exe, but path separators "
        "are backslashes and paths with spaces must be quoted.";
#elif defined(__APPLE__)
    constexpr const char* os_name  = "macOS (Darwin)";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `sw_vers` gives macOS version, `uname -a` gives kernel.";
#else
    constexpr const char* os_name  = "Linux";
    constexpr const char* shell    = "sh";
    constexpr const char* shell_hint =
        "Use POSIX tools; `/etc/os-release` gives distro info, `uname -a` gives kernel.";
#endif

    std::string cwd;
    // current_path().string() narrows the wide Windows path through the active
    // code page (ANSI), so a non-ASCII path turns into invalid UTF-8 and would
    // poison the whole system prompt on the JSON wire. u8string() converts the
    // wide path to UTF-8 directly — no lossy ANSI round-trip. (No-op on POSIX,
    // where the native encoding is already UTF-8.)
    try {
        auto u8 = std::filesystem::current_path().u8string();
        cwd.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (...) {}

    std::ostringstream oss;
    oss << "You are agentty, a terminal coding assistant. Act, don't ask. "
        << "When the user says something vague (\"edit it\", \"make it "
        << "better\", \"improve it\", \"make it interesting\", \"fix it\"), "
        << "make a reasonable improvement yourself with `edit` — do NOT "
        << "respond with a list of options or clarifying questions. Keep "
        << "prose short; let tool cards speak for themselves.\n\n"
        << "<file-editing>\n"
        << "  - For ANY change to a file that already exists, use `edit`. "
        << "If the file is in conversation history (you wrote it, or you "
        << "read it earlier), construct `edit.old_text` from memory — do "
        << "NOT re-read it.\n"
        << "  - `write` is for creating NEW files. If the file exists, "
        << "use `edit` — calling `write` on an existing file dumps the "
        << "entire body over the wire and stalls the stream; it is the "
        << "single worst latency choice available to you.\n"
        << "  - If a `write` fails with \"Output blocked by content "
        << "filtering policy\" (Anthropic's safety classifier — more "
        << "aggressive on OAuth / Pro / Max paths than on direct API "
        << "keys), you can: (a) retry once — the filter is "
        << "probabilistic, (b) write a short stub file first, then "
        << "build it up via successive `edit` calls. Don't loop on the "
        << "same large `write` more than twice.\n"
        << "  - `edit.old_text` must match the file exactly (indentation "
        << "matters; trailing whitespace is tolerated). If unsure, `read` "
        << "the relevant slice first.\n"
        << "  - NEVER shell out (cat/echo/sed/heredoc/printf) for file IO.\n"
        << "  - ALWAYS include a brief `display_description` on `write` "
        << "and `edit`. It paints in the tool card before the long fields "
        << "stream — schemas list `path` and `display_description` first "
        << "for that reason, don't reorder.\n"
        << "</file-editing>\n\n"
        << "<shell>\n"
        << "  - Use `bash` for commands. Explain destructive ones before "
        << "running.\n"
        << "  - For listing/searching files, prefer the dedicated tools "
        << "(`list_dir`, `glob`, `grep`, `find_definition`) over shelling "
        << "out — they give the UI structured cards.\n"
        << "</shell>\n\n"
        << "<output-formatting>\n"
        << "  - The TUI renders GFM markdown. A table MUST start its "
        << "header row at the line beginning with `|` and be preceded by "
        << "a blank line. NEVER put lead-in prose on the same line as the "
        << "header (`Layout: | Dir | Role |` renders as a wall of pipes — "
        << "the parser rejects it as a non-table). Write the lead-in as "
        << "its own line, then a blank line, then:\n"
        << "      | Dir | Role |\n"
        << "      |-----|------|\n"
        << "      | a/  | x    |\n"
        << "  - For 2-3 short columns, prefer a simple bulleted list over "
        << "a table — it reads better in a narrow terminal.\n"
        << "</output-formatting>\n\n"
        << "<context-economy>\n"
        << "  Every byte you ingest stays in context for the rest of "
        << "the session and pushes the conversation toward auto-"
        << "compaction. Be deliberate:\n"
        << "  - `read` returns up to 2000 lines. For larger files, "
        << "use `offset` + `limit` to page through — read only the "
        << "slice you need, not the whole file. Re-reading the SAME "
        << "(path, offset, limit) returns a 'file unchanged' sentinel "
        << "that you should respect: refer to the earlier tool_result "
        << "instead of re-fetching.\n"
        << "  - Prefer `grep` / `find_definition` over `read` when "
        << "you're looking for a pattern or symbol — they return the "
        << "match + 2 lines of context, not the whole file.\n"
        << "  - `bash` output is capped at 30 KB in your context. "
        << "Outputs larger than that are spilled to a temp file and "
        << "you receive a `<persisted-output>` envelope with a 2 KB "
        << "head + 1 KB tail and the file path. If you need bytes in "
        << "between, `read` the spill path with offset/limit — don't "
        << "re-run the command.\n"
        << "  - `web_fetch` is capped at 20 KB. For long pages, "
        << "fetch ONCE and remember what you saw; don't refetch the "
        << "same URL within a turn.\n"
        << "  - Don't ask for output you don't need. `ls -laR` of a "
        << "deep tree, a 50 K-line build log, or `find . -type f` in "
        << "node_modules will land in your context as one big tool "
        << "result and shorten the session for everyone.\n"
        << "</context-economy>\n\n"
        << "<environment>\n"
        << "  os: " << os_name << "\n"
        << "  shell: " << shell << "\n";
    if (!cwd.empty()) oss << "  cwd: " << cwd << "\n";
    oss << "</environment>\n\n"
        << "<shell-notes>\n"
        << shell_hint << "\n"
        << "</shell-notes>\n\n"
        << "<memory-tools>\n"
        << "  - If the user asks you to remember something — \"remember "
        << "that...\", \"don't forget X\", \"keep in mind Y\", \"from now "
        << "on...\", \"always do Z\" — you MUST call the `remember` tool. "
        << "Do not just acknowledge in prose; the prose disappears at the "
        << "end of the session, but `remember` persists to "
        << "~/.agentty/memory.jsonl (scope=user) or "
        << "<workspace>/.agentty/memory.jsonl (scope=project) and is "
        << "reloaded into your system prompt on every future turn.\n"
        << "  - Default scope is `project` (this codebase only). Use "
        << "scope=`user` when the fact is about the user themselves "
        << "(\"I prefer fish shell\", \"my name is...\", \"I use vim\") "
        << "and applies across every project.\n"
        << "  - Dedup is automatic: if you `remember` a fact that's "
        << "near-identical to an existing one in the same scope, the "
        << "store refreshes the existing record's timestamp + hit count "
        << "instead of writing a duplicate. Just call `remember` with "
        << "the fact; you don't need to grep <learned-memory> first.\n"
        << "  - Pass `pin=true` for facts the user has explicitly "
        << "emphasised (\"always do X\", \"never do Y\") or that are "
        << "load-bearing for every turn (the build command, a hard "
        << "project convention). Pinned facts survive cap rollover and "
        << "render with ★ in <learned-memory>.\n"
        << "  - Pass `tags=[\"build\", \"picker\"]` when a fact "
        << "belongs to an obvious topic. Tags group facts in the system "
        << "prompt so you can scan by area.\n"
        << "  - When the user CORRECTS a previous fact (\"actually the "
        << "build command is now Z\", \"that's no longer true\"), use "
        << "`remember` with `supersedes=<old-id>` — it atomically writes "
        << "the new record and drops the old one. Cleaner than "
        << "forget-then-remember.\n"
        << "  - Keep each remembered fact short and self-contained: one "
        << "sentence the future-you can act on without re-reading the "
        << "current conversation.\n"
        << "  - If the user asks you to forget something (\"forget X\", "
        << "\"that's no longer true\", \"drop the memory about Y\"), call "
        << "`forget` with either the record id (shown as `[id]` prefix "
        << "in the <learned-memory> block above) or a substring that "
        << "uniquely identifies the fact. Pass `dry_run=true` with a "
        << "substring first when the match might be broad — the tool "
        << "returns the list of records that WOULD be removed.\n"
        << "  - If the user wants a clean slate on this codebase (\"start "
        << "fresh\", \"forget everything you know about this project\", "
        << "\"wipe your memory\"), use `wipe_memory(scope=\"project\")`. "
        << "Call ONCE without `confirm` to preview the count; only after "
        << "the user agrees, re-call with `confirm=true`. `wipe_memory` "
        << "with scope=\"user\" wipes cross-project facts — require "
        << "explicit confirmation before doing that.\n"
        << "  - Do NOT call `remember` proactively for things the user "
        << "didn't ask you to remember. Don't store transient state "
        << "(current file you're editing, today's build error). Store "
        << "durable preferences and project conventions.\n"
        << "</memory-tools>\n";
    // Append CLAUDE.md tiers (User + Project + Local) when present.
    // Lives at the END of the prompt so the always-on rules above
    // anchor first; user-authored memory then layers on top.
    oss << collect_memory_blocks();
    // On-demand skills catalog (names + descriptions only). The full
    // bodies load lazily via the `skill` tool — progressive disclosure
    // keeps the per-request cost to one cheap line per skill.
    oss << tools::skills::catalog_block();
    return oss.str();
}

std::vector<ToolSpec> default_tools() {
    std::vector<ToolSpec> out;
    for (const auto& td : tools::wire_tools()) {
        out.push_back({td.name.value, td.description, td.input_schema});
    }
    return out;
}

// ----------------------------------------------------------------------------

void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    if (is_empty(req.auth)) {
        sink(StreamError{"not authenticated — run 'agentty login' or set ANTHROPIC_API_KEY"});
        return;
    }

    // emit_terminal runs on error paths after `sink` has been moved into
    // `ctx.sink` below — dispatching via `ctx.sink` is the only live handle.
    // The previous version captured `sink` by reference and invoked a
    // moved-from std::function on every non-happy-path termination, which
    // surfaced in the UI as "stream backend: bad_function_call".
    auto emit_terminal = [](StreamCtx& ctx, std::optional<std::string> err,
                             std::optional<std::chrono::seconds> retry_after = {}) {
        if (ctx.terminated) return;
        // If the stream is dying mid-tool-use (peer closed before the SSE
        // event sequence reached `content_block_stop`), synthesize a
        // StreamToolUseEnd so the reducer's salvage path runs on whatever
        // partial JSON we've buffered.
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
        if (err) ctx.sink(StreamError{*err, retry_after});
        else     ctx.sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    };

    const bool is_oauth = std::holds_alternative<BearerHeader>(req.auth);

    json body;
    body["model"]      = req.model;
    body["max_tokens"] = req.max_tokens;
    body["stream"]     = true;

    // GA-stable ephemeral cache breakpoint — no `ttl` (defaults to 5 min) and
    // no `scope` (defaults to per-organization). Claude Code's `Dt6` adds
    // `ttl:"1h"` when `extended-cache-ttl-2025-04-11` is in its beta header
    // set; we don't ride that beta, and sending `ttl:"1h"` without the gate
    // makes Anthropic's edge silently drop the breakpoint — every turn becomes
    // a cache miss and the stream gets routed through a throttled tier (~1-2
    // tok/s on opus). The 5 min default is plenty for a back-to-back REPL.
    const json kCacheCtl = {{"type", "ephemeral"}};

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
        sys.push_back({
            {"type", "text"},
            {"text", req.system_prompt},
            {"cache_control", kCacheCtl}
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
        tools_j.back()["cache_control"] = kCacheCtl;
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
        return;
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
            return;
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
        // ASCII case-fold compare for HTTP/2 (header names are already
        // lowercase per protocol, but be defensive against proxies).
        auto eq_ci = [](std::string_view a, std::string_view b) noexcept {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
                if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
                if (x != y) return false;
            }
            return true;
        };
        for (const auto& h : hh) {
            if (!eq_ci(h.name, "retry-after")) continue;
            // Anthropic always emits whole seconds; reject anything else
            // (the spec also allows HTTP-date, but Anthropic doesn't use
            // it and we'd rather fall back to our schedule than parse it
            // wrong). std::from_chars would be lighter; std::stoul is
            // fine on a header value bounded at a few digits.
            try {
                size_t consumed = 0;
                auto v = std::stoul(h.value, &consumed);
                if (consumed == h.value.size() && v > 0) {
                    retry_after_hint = std::chrono::seconds(v);
                }
            } catch (...) {
                // ignored — leave hint unset, runtime falls back to its
                // own schedule.
            }
            break;
        }
    };
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
    tos.total   = std::chrono::milliseconds(0);  // streaming phase unbounded
    // A healthy Anthropic stream emits SSE `ping` heartbeats every 10-15 s
    // even during long thinking blocks. 90 s without a single byte means
    // the transport is dead (silent peer, proxy stall, half-open TCP).
    // The error surfaces as "h2: idle timeout (no bytes for Ns)" and is
    // classified as Transient by provider::error_class — auto-retried
    // with backoff.
    //
    // The 90 s value is deliberately more patient than the historical
    // 45 s: on heavily-loaded Anthropic edge pops we've observed
    // legitimate 30-60 s ping intervals before the connection recovers,
    // and an aggressive HTTP idle timer converted those brown-outs into
    // user-visible "stream stalled" errors. The reducer has its own
    // 120 s stall watchdog that catches the case where PING ACKs keep
    // this clock happy but the application layer never advances — the
    // two watchdogs together cover both failure modes without racing.
    //
    // 15 s PING probe interval keeps a half-open TCP from going
    // undetected for long; the PING ACK bumps last_rx so a healthy peer
    // never trips idle.
    tos.ping    = std::chrono::milliseconds(15'000);
    tos.idle    = std::chrono::milliseconds(90'000);

    auto result = http::default_client().stream(hreq, std::move(handler),
                                                tos, std::move(cancel));

    dbg("==== http status=%d transport=%s thinking_deltas=%d ====\n",
        http_status, result ? "ok" : result.error().render().c_str(),
        ctx.thinking_deltas);

    if (!result) {
        // Network / TLS / nghttp2-level error — never produced a complete SSE
        // stream. The typed HttpError carries a `kind` that the downstream
        // classifier reads structurally; we ALSO embed `render()` in the
        // detail string so the error_class fallback path (substring sniff)
        // still works for messages that haven't been routed yet.
        emit_terminal(ctx, std::string{"http: "} + result.error().render());
        return;
    }

    if (!is_success) {
        dbg("error body: %s\n", error_body.c_str());
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
        emit_terminal(ctx, std::move(msg), retry_after_hint);
        return;
    }

    // 2xx — the SSE parser may or may not have produced message_stop.
    // Guarantee one terminal event so the UI can finalize the turn.
    emit_terminal(ctx, std::nullopt);
}

std::vector<ModelInfo> list_models(const AuthHeader& auth) {
    // Built-in catalog. Anthropic's ids are stable and few, so unlike the
    // OpenAI/Ollama path we always have a trustworthy fallback. Returned
    // only when the network probe genuinely yields nothing (offline, or a
    // transient non-200) so the picker is never stranded empty. With valid
    // creds the real /v1/models below returns the full upstream catalog —
    // the seed is just the floor, not the ceiling.
    auto seed = [] {
        return std::vector<ModelInfo>{
            ModelInfo{ModelId{"claude-opus-4-5"},   "Claude Opus 4.5",   "anthropic", 200000, true},
            ModelInfo{ModelId{"claude-sonnet-4-5"}, "Claude Sonnet 4.5", "anthropic", 200000, true},
            ModelInfo{ModelId{"claude-haiku-4-5"},  "Claude Haiku 4.5",  "anthropic", 200000, false},
        };
    };

    std::vector<ModelInfo> result;
    if (is_empty(auth)) return seed();

    const bool is_oauth = std::holds_alternative<BearerHeader>(auth);

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
            });
        }
    } catch (...) {}

    // Network said 200 but we parsed nothing usable — fall back to the seed
    // so the picker is never left empty after a provider switch.
    if (result.empty()) return seed();
    return result;
}

} // namespace agentty::provider::anthropic
