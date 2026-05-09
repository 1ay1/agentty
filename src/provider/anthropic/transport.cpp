#include "moha/provider/anthropic/transport.hpp"

#include <algorithm>
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

#include "moha/domain/catalog.hpp"
#include "moha/io/http.hpp"
#include "moha/tool/registry.hpp"
#include "moha/util/base64.hpp"

namespace moha::provider::anthropic {

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

// Env-var-gated request/SSE dump. Set MOHA_DEBUG_API=1 to write to
// $MOHA_DEBUG_FILE (or ./moha-api.log). Appends, never truncates.
FILE* debug_log() {
    static std::mutex m;
    static FILE* fp = nullptr;
    static bool tried = false;
    std::lock_guard<std::mutex> lk(m);
    if (tried) return fp;
    tried = true;
    const char* on = std::getenv("MOHA_DEBUG_API");
    if (!on || !*on || *on == '0') return nullptr;
    const char* path = std::getenv("MOHA_DEBUG_FILE");
    std::string p = (path && *path) ? std::string{path} : std::string{"moha-api.log"};
    fp = std::fopen(p.c_str(), "ab");
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
struct SseState {
    // Pre-reserve so typical chunk sizes don't force a cascade of reallocations
    // during a fast stream.
    SseState() { buf.reserve(64 * 1024); data_accum.reserve(8 * 1024); }
    std::string buf;
    // Offset of the next byte in `buf` to parse. Lets feed_sse() drain
    // already-parsed lines without an O(n) `erase(0, pos)` memmove on every
    // chunk — we only compact `buf` once the read offset crosses a fairly
    // large threshold (kCompactThreshold below). On a hot stream this turns
    // the per-chunk drain cost from O(buffered bytes) to amortized O(1).
    std::size_t read_pos = 0;
    std::string event_name;
    std::string data_accum;
};
// Compact the SSE buffer when the read cursor has consumed at least this
// many bytes. A larger threshold means more memory held at the high-water
// mark, but fewer memmove passes. 64 KiB sits a couple chunk-sizes ahead
// of typical Anthropic SSE frames; the buffer never grows unbounded
// because the total in-flight tail (current event boundary → end of buf)
// is small even on busy streams.
constexpr std::size_t kSseCompactThreshold = 64 * 1024;

// Hard ceiling on the multi-line `data:` accumulator within a single SSE
// event. Real Anthropic frames are 1-10 KB; 4 MiB is generously past the
// largest content_block_delta we've ever observed. A misbehaving server
// emitting an unbounded data: stream would otherwise fill `data_accum`
// until OOM. When the cap is hit we drop the in-flight event silently
// and the next blank line resets the accumulator.
constexpr std::size_t kSseDataAccumMax = 4 * 1024 * 1024;

struct StreamCtx {
    EventSink sink;
    SseState sse;
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
bool dispatch_content_block_delta_fast(StreamCtx& ctx, const std::string& data) {
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
    if (delta_type == "thinking_delta" || delta_type == "signature_delta") {
        ++ctx.thinking_deltas;
        ctx.sink(StreamHeartbeat{});
        return true;
    }
    return false;
}

void dispatch_event(StreamCtx& ctx, std::string_view name, const std::string& data) {
    if (data.empty() || data == "[DONE]") return;
    // dbg() format string is %s — copy through a small stack buffer only
    // when the debug log is actually enabled (debug_log() returns nullptr
    // otherwise, in which case dbg() short-circuits and `name` is never
    // touched). Avoids constructing a std::string per event in the hot path.
    if (debug_log()) {
        std::string name_owned{name};
        dbg("<< event=%s data=%s\n", name_owned.c_str(), data.c_str());
    }

    // Hot path first — ~95% of events during a streaming turn.
    if (name == "content_block_delta"
        && dispatch_content_block_delta_fast(ctx, data)) {
        return;
    }

    // ping events are heartbeat keepalives — Anthropic interleaves them so
    // proxies don't kill the long-poll (typically every 10-15 s). Forward
    // as a StreamHeartbeat so the reducer's stall watchdog can tell
    // "wire is silent but alive" from "wire is wedged." The reducer's
    // handler only bumps last_event_at — no render, no state change.
    if (name == "ping") { ctx.sink(StreamHeartbeat{}); return; }

    json j;
    try { j = json::parse(data); } catch (...) { return; }
    if (name == "message_start") {
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
    } else if (name == "content_block_start") {
        auto block = j.value("content_block", json::object());
        auto type = block.value("type", "");
        if (type == "tool_use") {
            ctx.current_tool_id = block.value("id", "");
            ctx.current_tool_name = block.value("name", "");
            ctx.in_tool_use = true;
            ctx.sink(StreamToolUseStart{ToolCallId{ctx.current_tool_id}, ToolName{ctx.current_tool_name}});
        }
    } else if (name == "content_block_delta") {
        auto delta = j.value("delta", json::object());
        auto type = delta.value("type", "");
        if (type == "text_delta") {
            ctx.sink(StreamTextDelta{delta.value("text", "")});
        } else if (type == "input_json_delta") {
            ctx.sink(StreamToolUseDelta{delta.value("partial_json", "")});
        } else if (type == "thinking_delta" || type == "signature_delta") {
            // Extended-thinking models can emit thinking_delta blocks even
            // when we don't enable thinking via the request body — Anthropic
            // routes some opus turns through an implicit reasoning pass. We
            // don't render thinking content in the UI yet, but emitting a
            // StreamHeartbeat here is what keeps the reducer's stall
            // watchdog from tripping: the model can reason silently for
            // minutes at a time, and without a liveness signal the
            // reducer can't distinguish that from a wedged transport.
            ++ctx.thinking_deltas;
            ctx.sink(StreamHeartbeat{});
        }
    } else if (name == "content_block_stop") {
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
    } else if (name == "message_delta") {
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
    } else if (name == "message_stop") {
        if (ctx.in_tool_use) {
            ctx.sink(StreamToolUseEnd{});
            ctx.in_tool_use = false;
            ctx.current_tool_id.clear();
            ctx.current_tool_name.clear();
        }
        ctx.sink(StreamFinished{ctx.stop_reason});
        ctx.terminated = true;
    } else if (name == "error") {
        // Mid-stream SSE error event. The wire payload is just `error.type`
        // + `error.message` — Anthropic doesn't surface Retry-After here
        // (it's an HTTP-header thing, and we already passed the headers
        // phase to enter the SSE body). Leave retry_after unset and let
        // the runtime fall back to its own schedule.
        auto err = j.value("error", json::object());
        ctx.sink(StreamError{err.value("message", "unknown error"), std::nullopt});
        ctx.terminated = true;
    }
}

void feed_sse(StreamCtx& ctx, const char* data, size_t len) {
    ctx.sse.buf.append(data, len);
    auto& read_pos = ctx.sse.read_pos;
    // Treat the buffer as a string_view so per-line work doesn't allocate.
    // We only own state in `ctx.sse.event_name` and `data_accum`; lines
    // are walked as views into `ctx.sse.buf` directly.
    std::string_view buf{ctx.sse.buf};
    while (true) {
        const auto nl = buf.find('\n', read_pos);
        if (nl == std::string_view::npos) break;
        std::string_view line = buf.substr(read_pos, nl - read_pos);
        read_pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (line.empty()) {
            if (!ctx.sse.data_accum.empty() || !ctx.sse.event_name.empty())
                dispatch_event(ctx, ctx.sse.event_name, ctx.sse.data_accum);
            ctx.sse.event_name.clear();
            ctx.sse.data_accum.clear();
        } else if (line.starts_with("event:")) {
            std::size_t s = 6;
            while (s < line.size() && line[s] == ' ') ++s;
            // Reuse storage in event_name to avoid the per-event std::string
            // allocation that the previous substr() path forced.
            ctx.sse.event_name.assign(line.data() + s, line.size() - s);
        } else if (line.starts_with("data:")) {
            std::size_t s = 5;
            while (s < line.size() && line[s] == ' ') ++s;
            const std::size_t add = (line.size() - s) +
                (ctx.sse.data_accum.empty() ? 0 : 1);
            if (ctx.sse.data_accum.size() + add > kSseDataAccumMax) {
                // Defensive cap: drop the in-flight event silently
                // rather than grow the accumulator without bound.
                ctx.sse.data_accum.clear();
                ctx.sse.event_name.clear();
                continue;
            }
            if (!ctx.sse.data_accum.empty()) ctx.sse.data_accum.push_back('\n');
            ctx.sse.data_accum.append(line.data() + s, line.size() - s);
        }
        // Anything else (`:` comments, unknown fields) is silently dropped
        // — matches the SSE spec and Claude's transport.
    }
    // Amortized drain: only compact the buffer when the read cursor has
    // consumed enough bytes that the cost of memmove is paid back. Until
    // then, leave already-parsed bytes in place.
    if (read_pos >= kSseCompactThreshold) {
        ctx.sse.buf.erase(0, read_pos);
        read_pos = 0;
    }
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
// over this with a "Thinking…" spinner; moha's TUI doesn't, so it just looks
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
http::Headers build_request_headers(bool is_oauth,
                                    const std::string& auth_header,
                                    std::string_view beta_value,
                                    int timeout_seconds,
                                    bool streaming = false) {
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
    h.push_back({"x-stainless-retry-count",     "0"});
    h.push_back({"x-stainless-timeout",         std::to_string(timeout_seconds)});
    if (streaming) {
        // .stream() helpers in cli.js always set helper-method=stream; the
        // sibling x-stainless-helper carries the agent-loop tag so Anthropic
        // can distinguish raw API consumers from the official tool runner.
        h.push_back({"x-stainless-helper-method", "stream"});
        h.push_back({"x-stainless-helper",        "BetaToolRunner"});
    }
    if (is_oauth) h.push_back({"authorization", auth_header});
    else          h.push_back({"x-api-key",     auth_header});
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
        if (seed.empty()) seed = "moha-anonymous";
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
    // — moha shipped that and it correlated with a 20-30 s mid-stream pause
    // on long tool_use bodies. Anthropic's edge appears to inspect this field
    // for routing/quota; matching the new shape byte-for-byte is part of the
    // fix. We don't own a real account UUID under OAuth here, so we leave it
    // empty exactly as CC does when one isn't available.
    auto device_id  = machine_id_hex(32);
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t s1 = 0xcbf29ce484222325ull;
    for (int i = 0; i < 8; ++i) { s1 ^= (now >> (i*8)) & 0xff; s1 *= 0x100000001b3ull; }
    uint64_t s2 = s1 ^ 0x9e3779b97f4a7c15ull;
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)s1, (unsigned long long)s2);
    auto session_id = std::string(buf, 32);
    return nlohmann::json{
        {"device_id",    device_id},
        {"account_uuid", ""},
        {"session_id",   session_id},
    }.dump();
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

[[nodiscard]] inline bool is_assistant_with_results(const Message& m) noexcept {
    if (m.role != Role::Assistant || m.tool_calls.empty()) return false;
    for (const auto& tc : m.tool_calls) if (tc.is_terminal()) return true;
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
    auto raw_output = tc.output();
    std::string scrubbed;
    if (raw_output.empty()) {
        json_write_field(out, "content", "(no output)", first);
    } else {
        scrubbed = scrub_utf8(raw_output);
        json_write_field(out, "content", scrubbed, first);
    }
    json_write_bool_field(out, "is_error", tc.is_failed() || tc.is_rejected(), first);
    if (pin_cache) json_write_raw_field(out, "cache_control", kCacheCtlJsonRaw, first);
    out.push_back('}');
}

} // namespace

[[nodiscard]] std::string messages_json_string(const Thread& t) {
    // First pass: figure out where the cache breakpoints land. cli.js
    // pins BOTH the last and second-to-last *emitted* messages' last
    // content blocks (rolling cache reuse — turn N's last becomes turn
    // N+1's second-to-last). A "message" here is whatever lands in the
    // output array, so an Assistant turn with terminal tool_calls
    // contributes TWO messages (assistant + tool_results follow-up).
    int total_msgs = 0;
    for (const auto& m : t.messages) {
        const bool has_images = (m.role == Role::User && !m.images.empty());
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
        const bool has_images = (m.role == Role::User && !m.images.empty());
        const bool has_tools  = (m.role == Role::Assistant && !m.tool_calls.empty());
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
            int blocks = (has_images ? static_cast<int>(m.images.size()) : 0)
                       + (has_text ? 1 : 0)
                       + (has_tools ? static_cast<int>(m.tool_calls.size()) : 0);
            int block_emitted = 0;
            if (has_images) {
                for (const auto& img : m.images) {
                    if (block_emitted++ > 0) out.push_back(',');
                    const bool last_block = (block_emitted == blocks);
                    write_image_block(out, img, do_pin && last_block);
                }
            }
            if (has_text) {
                if (block_emitted++ > 0) out.push_back(',');
                const bool last_block = (block_emitted == blocks);
                write_text_block(out, scrub_utf8(m.text), do_pin && last_block);
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
        if (is_assistant_with_results(m)) {
            const int my_idx   = emitted;
            const bool do_pin  = pinning_for(my_idx);
            emit_msg_open();
            out.append(R"({"role":"user","content":[)");
            int total_results = 0;
            for (const auto& tc : m.tool_calls) if (tc.is_terminal()) ++total_results;
            int result_emitted = 0;
            for (const auto& tc : m.tool_calls) {
                if (!tc.is_terminal()) continue;
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
    return json::parse(messages_json_string(t));
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

    if (user.empty() && project.empty() && local.empty()) return {};

    std::ostringstream m;
    m << "\n\n<memory>\n"
      << "Project-specific guidance the user has authored. Treat these "
         "as persistent context for THIS workspace and user; lower tiers "
         "(local, then project, then user) win on conflicting rules.\n";
    if (!user.empty())    m << "<user-memory>\n"    << user    << "\n</user-memory>\n";
    if (!project.empty()) m << "<project-memory>\n" << project << "\n</project-memory>\n";
    if (!local.empty())   m << "<local-memory>\n"   << local   << "\n</local-memory>\n";
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
    try { cwd = std::filesystem::current_path().string(); } catch (...) {}

    std::ostringstream oss;
    oss << "You are Moha, a terminal coding assistant. Act, don't ask. "
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
        << "<environment>\n"
        << "  os: " << os_name << "\n"
        << "  shell: " << shell << "\n";
    if (!cwd.empty()) oss << "  cwd: " << cwd << "\n";
    oss << "</environment>\n\n"
        << "<shell-notes>\n"
        << shell_hint << "\n"
        << "</shell-notes>\n";
    // Append CLAUDE.md tiers (User + Project + Local) when present.
    // Lives at the END of the prompt so the always-on rules above
    // anchor first; user-authored memory then layers on top.
    oss << collect_memory_blocks();
    return oss.str();
}

std::vector<ToolSpec> default_tools() {
    std::vector<ToolSpec> out;
    for (const auto& td : tools::registry()) {
        out.push_back({td.name.value, td.description, td.input_schema});
    }
    return out;
}

// ----------------------------------------------------------------------------

void run_stream_sync(Request req, EventSink sink, http::CancelTokenPtr cancel) {
    if (req.auth_header.empty()) {
        sink(StreamError{"not authenticated — run 'moha login' or set ANTHROPIC_API_KEY"});
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

    const bool is_oauth = (req.auth_style == auth::Style::Bearer);

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
    std::string messages_str = messages_json_string(
        Thread{ThreadId{""}, "", req.messages, {}, {}});
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
    // Splice marker for the messages array. nlohmann gives the dumped
    // form `"messages":<unique-string>"`; we string-replace the
    // placeholder with messages_json_string. Picked a token that
    // can't appear inside a legitimately-escaped JSON string so the
    // find() is unambiguous even on weird payloads.
    constexpr std::string_view kMessagesPlaceholder =
        "\x01__moha_messages_splice__\x01";
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
    // dumped form is `"__moha_messages_splice__"` — find
    // and replace that.
    {
        constexpr std::string_view kDumpedPlaceholder =
            "\"\\u0001__moha_messages_splice__\\u0001\"";
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
    if (const auto& ov = http::moha_api_host_override(); ov.active()) {
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
    hreq.headers = build_request_headers(is_oauth, req.auth_header,
                                         select_betas(req.model, is_oauth, any_eager),
                                         /*timeout_seconds=*/300,
                                         /*streaming=*/true);
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
            msg += "  (run 'moha login' to re-authenticate)";
        emit_terminal(ctx, std::move(msg), retry_after_hint);
        return;
    }

    // 2xx — the SSE parser may or may not have produced message_stop.
    // Guarantee one terminal event so the UI can finalize the turn.
    emit_terminal(ctx, std::nullopt);
}

std::vector<ModelInfo> list_models(const std::string& auth_header,
                                   auth::Style auth_style) {
    std::vector<ModelInfo> result;
    if (auth_header.empty()) return result;

    const bool is_oauth = (auth_style == auth::Style::Bearer);

    http::Request hreq;
    hreq.method  = http::HttpMethod::Get;
    hreq.host    = "api.anthropic.com";
    hreq.port    = 443;
    if (const auto& ov = http::moha_api_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.path    = "/v1/models?limit=100";
    // /v1/models doesn't need the streaming beta cocktail — just the oauth
    // gate when applicable, matching how cli.js calls model-listing endpoints.
    hreq.headers = build_request_headers(is_oauth, auth_header,
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
    if (!resp || resp->status != 200) return result;

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

    return result;
}

} // namespace moha::provider::anthropic
