#pragma once

// ── Provider stream scaffold: the shared "how a streamed turn RUNS" layer ────
//
// stream_epilogue.hpp owns how a turn ENDS. This header owns the mirror-image
// duplication at the START and MIDDLE: every streaming transport (Anthropic,
// OpenAI-compat, Ollama-native, ChatGPT/Codex/Copilot Responses) hand-rolled
// the same StreamHandler scaffolding —
//
//   • capture the HTTP status, parse Retry-After on the error path
//   • forward transport activity as transport-only StreamHeartbeats
//   • forward TCP-buffered sends as StreamBufferedWait
//   • dump each chunk on the wire channel, tagged with the dialect
//   • on success feed the dialect parser; on error accumulate the body
//     under a 64 KB cap (a misbehaving edge streaming an unbounded 4xx
//     body must not OOM the error path)
//   • after the loop, log ONE end-of-turn result line + the raw error body
//
// and they drifted, exactly as the epilogue's transports once did: Ollama
// lost on_buffered_wait (buffered-send stalls showed as dead air), the
// error-path predicate diverged (is_success vs status>=400), and the
// end-of-turn log lines took three different shapes. This header is the
// single source of truth; a transport supplies ONLY what genuinely differs:
// the dialect tag, the parser feed, and (optionally) how long to wait.
//
// Deliberately header-only and allocation-free on the hot path, same as the
// epilogue.

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "agentty/io/http.hpp"
#include "agentty/provider/debug.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"   // parse_retry_after
#include "agentty/runtime/msg.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::provider {

// Error bodies are buffered for diagnostics but capped: parity across every
// transport, pinned by provider_conformance_test.
inline constexpr std::size_t kErrorBodyCap = 64 * 1024;

// The standard streaming timeout ladder. Values are the measured/argued
// consensus (see the long comments that used to sit in each transport):
//   connect 10 s   — TCP+TLS to a healthy host is <1 s; 10 s covers a slow
//                    corporate path without wedging startup-adjacent calls.
//   total   30 min — a responsive gateway can buffer every SSE frame; give
//                    legitimate large turns ample time but never wedge
//                    forever.
//   ping    15 s   — HTTP/2 PING probes keep a half-open TCP detectable and
//                    produce transport-only heartbeats when a gateway is
//                    alive but withholding DATA.
//   idle    90 s   — a healthy hosted stream emits deltas/keepalives
//                    continuously; 90 s of silence is a dead transport
//                    (silent peer, proxy stall, half-open TCP). Classified
//                    Transient → auto-retried with backoff.
//
// `idle` is a parameter because it is the one knob with a legitimate
// per-transport story: LOCAL model servers (llama.cpp, Ollama) send NOTHING
// during prompt processing, and a big model on consumer hardware can grind
// for minutes before the first token — a 90 s cut mid-processing forces a
// retry that re-processes the same prompt from scratch, forever.
[[nodiscard]] inline http::Timeouts stream_timeouts(
        std::chrono::milliseconds idle = std::chrono::milliseconds(90'000)) {
    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::minutes(30);
    tos.ping    = std::chrono::milliseconds(15'000);
    tos.idle    = idle;
    return tos;
}

// Per-turn scaffold state + the uniform StreamHandler. Stack-allocate one per
// run_stream_sync, wire `handler()` into the HTTP client, and read the
// captured fields in the post-loop / finish_stream call.
struct StreamScaffold {
    // What the transport supplies.
    const char* dialect;            // "anthropic-messages", "openai-chat", …
    EventSink   sink;               // heartbeats / buffered-wait land here
    // Feed one success-path chunk to the dialect parser. Return false to
    // stop reading (e.g. the body already fired its terminal event — the
    // Responses codec's deliberate post-`response.completed` abort).
    std::function<bool(std::string_view chunk)> feed;

    // What the scaffold captures for the post-loop.
    int         http_status = 0;
    std::string error_body;         // capped at kErrorBodyCap
    std::optional<std::chrono::seconds> retry_after_hint;

    [[nodiscard]] bool ok() const noexcept {
        return http_status >= 200 && http_status < 300;
    }

    // Build the uniform handler. `this` must outlive the stream call (it
    // lives on run_stream_sync's stack; the HTTP client is synchronous).
    [[nodiscard]] http::StreamHandler handler() {
        http::StreamHandler h;
        h.on_headers = [this](int status, const http::Headers& hh) {
            http_status = status;
            AGT_LOG(Wire, Debug, "stream.response", "dialect={} status={}",
                    dialect, status);
            if (ok()) return;
            retry_after_hint = parse_retry_after(hh);
        };
        h.on_activity = [this] {
            sink(StreamHeartbeat{.transport_only = true});
        };
        h.on_buffered_wait = [this] { sink(StreamBufferedWait{}); };
        h.on_chunk = [this](std::string_view chunk) -> bool {
            // VERBATIM, untruncated: the parser is often the thing under
            // suspicion, so a clipped frame hides exactly the bytes that
            // matter (the Copilot `.done` event carrying tool arguments sat
            // past any reasonable truncation point in a real turn).
            dbg_chunk(dialect, chunk);
            if (ok()) return feed(chunk);
            if (error_body.size() < kErrorBodyCap)
                error_body.append(chunk.data(),
                    std::min(chunk.size(), kErrorBodyCap - error_body.size()));
            return true;   // keep draining the error body up to the cap
        };
        return h;
    }

    // The uniform end-of-turn pair: one Debug summary + the raw error body
    // at Warn when the turn failed. THE lines a shared log answers "why did
    // my provider fail?" with. `transport_ok` = the HTTP client returned
    // success; `detail` = optional dialect-specific counters
    // ("thinking_deltas=3"), already formatted.
    void log_result(bool transport_ok, std::string_view transport_err,
                    std::string_view detail = {}) const {
        AGT_LOG(Wire, Debug, "stream.result",
                "dialect={} status={} transport={} {}",
                dialect, http_status,
                transport_ok ? std::string_view{"ok"} : transport_err, detail);
        if ((!transport_ok || !ok()) && !error_body.empty())
            AGT_LOG(Wire, Warn, "stream.error.body", "dialect={} status={} raw={}",
                    dialect, http_status, error_body);
    }
};

} // namespace agentty::provider
