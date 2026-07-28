#pragma once

// ── Provider stream epilogue: the shared "how a streamed turn ends" layer ────
//
// Every streaming transport (Anthropic, OpenAI-compat, Ollama, ChatGPT/Codex)
// faces the SAME two problems once its SSE/NDJSON body has been fed to the
// HTTP client:
//
//   1. Emit EXACTLY ONE terminal event. A turn finishes with a single
//      StreamFinished (success) or StreamError (failure) — never zero, never
//      two. The body itself usually fires it (a `response.completed` /
//      `message_stop` / `[DONE]` frame), so the post-loop code must NOT emit a
//      second one. Forgetting this guard is exactly the bug the ChatGPT path
//      had: it aborted the read after `response.completed`, the HTTP layer
//      reported that intentional abort as "cancelled", and the post-loop
//      forwarded it as a spurious StreamError — so a clean turn showed
//      "cancelled".
//
//   2. Interpret the loop exit uniformly. When the body did NOT already finish,
//      the reason the stream loop returned has a fixed precedence: a real user
//      cancel, then an HTTP error status, then a transport error, else a clean
//      socket close (finish with the last-seen stop reason).
//
// Historically each transport open-coded both, and they drifted (the ChatGPT
// path got #1 wrong). This header is the single source of truth so all
// providers behave identically and a new transport gets it right for free.
//
// It is deliberately tiny and header-only: two free functions over the common
// vocabulary (EventSink, StopReason, StreamFinished/StreamError, CancelToken).
// Transport-specific "flush before finishing" work (salvage a leaked tool call,
// close an open tool block) is injected as a callback, so the shared rule holds
// without the shared code needing to know each transport's internals.

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "agentty/io/http.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::provider {

// Emit the single terminal event for a turn, exactly once.
//
//   terminated    in/out latch. If already true this is a no-op (the body — or
//                 an earlier epilogue call — already finished the turn). Set to
//                 true on emit so a later call can't double-finish.
//   sink          the transport's EventSink.
//   stop          stop reason to report on SUCCESS (ignored on error).
//   err           std::nullopt → success (StreamFinished{stop}); otherwise
//                 StreamError{*err, retry_after}.
//   retry_after   server Retry-After hint to carry on an error (429/529).
//   before_finish transport hook run ONLY on the success path, just before
//                 StreamFinished — the place to flush held text / close an open
//                 tool block / salvage a leaked tool call. Not run on error.
//
// This is the shared body of every transport's old `emit_terminal` lambda.
inline void finish_turn_once(
        bool& terminated,
        const EventSink& sink,
        StopReason stop,
        std::optional<std::string> err = std::nullopt,
        std::optional<std::chrono::seconds> retry_after = std::nullopt,
        const std::function<void()>& before_finish = {}) {
    if (terminated) return;
    terminated = true;
    if (err) {
        StreamError e{std::move(*err), retry_after};
        sink(std::move(e));
    } else {
        if (before_finish) before_finish();
        sink(StreamFinished{stop});
    }
}

// How a stream loop ended, in precedence order. `classify_stream_end` returns
// exactly one; the transport switches on it to emit the right terminal event
// (or nothing, for AlreadyTerminated).
enum class StreamEnd {
    // The SSE/NDJSON body already fired a terminal event (and set `terminated`).
    // Emit NOTHING more — this is the case that must win, so an intentional
    // read-abort after a clean finish is never mistaken for a cancel/error.
    AlreadyTerminated,
    // The caller's cancel token is set: a genuine user Esc. Emit
    // StreamError{"cancelled"}.
    UserCancelled,
    // Response status was >= 400. The transport builds the error message from
    // the buffered error body.
    HttpError,
    // The HTTP client returned an error (connection/TLS/reset) and it was not a
    // user cancel. The transport renders result.error().
    TransportError,
    // A 2xx stream closed cleanly without an explicit terminal frame (proxy
    // cutoff). Finish with the last-seen stop reason.
    CleanClose,
};

// Classify the loop exit. `result_ok` is `bool(result)` for the transport's
// std::expected return; `http_status` is the observed response status (0 if
// headers never arrived — treated as not-an-HTTP-error so a transport error or
// cancel wins). Precedence is fixed and identical for every provider.
[[nodiscard]] inline StreamEnd classify_stream_end(
        bool terminated,
        bool result_ok,
        int http_status,
        const http::CancelTokenPtr& cancel) noexcept {
    if (terminated)                             return StreamEnd::AlreadyTerminated;
    if (cancel && cancel->is_cancelled())       return StreamEnd::UserCancelled;
    if (http_status >= 400)                     return StreamEnd::HttpError;
    if (!result_ok)                             return StreamEnd::TransportError;
    return StreamEnd::CleanClose;
}

} // namespace agentty::provider
