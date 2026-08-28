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
#include "agentty/provider/debug.hpp"        // shared AGENTTY_DEBUG_API logger
#include "agentty/provider/provider.hpp"     // EventSink
#include "agentty/provider/wire.hpp"         // wire::SseFramer
#include "agentty/runtime/msg.hpp"
#include "agentty/util/env.hpp"

namespace agentty::provider::anthropic {

// Env-var-gated request/SSE dump. Set AGENTTY_DEBUG_API=1 to write to
// $AGENTTY_DEBUG_FILE (or ./agentty-api.log). Appends, never truncates.
// Shared by the parser (per-event trace) and run_stream_sync (request +
// status trace) — inline so both TUs see one definition.
// Env-var-gated request/SSE dump. Set AGENTTY_DEBUG_API=1 to write to
// $AGENTTY_DEBUG_FILE (or ./agentty-api.log). Appends, never truncates.
// Now delegates to the SHARED provider/debug.hpp logger (one file, every
// transport); these aliases keep the existing anthropic:: call sites.
using provider::debug_log;
using provider::dbg;

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
