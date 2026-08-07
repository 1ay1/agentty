#pragma once
// agentty::provider::anthropic — SSE parse internals (shared between
// transport.cpp's run_stream_sync/parse_sse_for_test and sse.cpp's parser
// definitions). INTERNAL header: not part of the public provider surface.
//
// Holds the streaming context every SSE frame mutates (StreamCtx), the
// feed_sse entry point, and the env-gated debug logger (dbg/debug_log) which
// is used by BOTH the parser and the request driver, so it lives here as
// inline to avoid a cross-TU definition. dispatch_event and the simdjson fast
// path stay private to sse.cpp — run_stream_sync only ever calls feed_sse.

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>

#include <simdjson.h>

#include "agentty/domain/catalog.hpp"        // StopReason
#include "agentty/provider/provider.hpp"     // EventSink
#include "agentty/provider/wire.hpp"         // wire::SseFramer
#include "agentty/runtime/msg.hpp"
#include "agentty/util/env.hpp"

namespace agentty::provider::anthropic {

// Env-var-gated request/SSE dump. Set AGENTTY_DEBUG_API=1 to write to
// $AGENTTY_DEBUG_FILE (or ./agentty-api.log). Appends, never truncates.
// Shared by the parser (per-event trace) and run_stream_sync (request +
// status trace) — inline so both TUs see one definition.
inline FILE* debug_log() {
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

inline void dbg(const char* fmt, ...) {
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

// Per-stream parse state. One instance lives for the duration of a single
// /v1/messages stream; every SSE frame mutates it via dispatch_event (in
// sse.cpp). run_stream_sync constructs it, wires ctx.sink, then drives
// feed_sse per network chunk and reads the terminal fields after.
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

// Feed one network read's worth of SSE bytes. Owns nothing: framing +
// buffering live in ctx.sse (wire::SseFramer); each complete event is
// dispatched through dispatch_event (sse.cpp). Defined in sse.cpp.
void feed_sse(StreamCtx& ctx, const char* data, std::size_t len);

} // namespace agentty::provider::anthropic
