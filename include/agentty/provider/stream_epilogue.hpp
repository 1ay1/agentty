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
        const std::function<void()>& before_finish = {},
        int http_status = 0) {
    if (terminated) return;
    terminated = true;
    if (err) {
        StreamError e{std::move(*err), retry_after};
        e.http_status = http_status;
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

// ── StreamResult: how a streamed turn ended, as a VALUE ────────────────────
//
// The terminal outcome of a turn used to be observable only as a side effect —
// a StreamFinished / StreamError Msg pushed into the sink, which a caller had
// to intercept and re-decode to learn "did this turn error? was it cancelled?
// what was the retry hint?". StreamResult makes that outcome a first-class
// return value: `stream()` (and finish_stream) hand it back, the emitted
// terminal Msg is DERIVED from it, and callers read a field instead of sniffing
// events.
//
// (Distinct from provider::TurnResult in acp_backend.hpp, which is the ACP
// backend abstraction's per-ROUND result carrying a richer TurnError. This is
// the in-process SSE/NDJSON epilogue's outcome, keyed on StreamEnd + the raw
// HTTP status — the two describe adjacent layers and are intentionally not the
// same type.)
//
// `end` is the classified precedence winner (see classify_stream_end). The
// derived accessors below fold the StreamEnd + payload into the two questions
// callers actually ask.
struct StreamResult {
    StreamEnd                             end = StreamEnd::CleanClose;
    StopReason                            stop = StopReason::EndTurn;
    std::optional<std::string>            error;        // set iff the turn failed
    std::optional<std::chrono::seconds>   retry_after;  // server hint on 429/529
    int                                   http_status = 0;

    // Did the turn end successfully (clean close, or the body already finished
    // with a terminal we respect)? False for cancel / HTTP / transport error.
    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
    // Was this a user-initiated cancel (vs. a genuine failure)?
    [[nodiscard]] bool cancelled() const noexcept {
        return end == StreamEnd::UserCancelled;
    }
    // Did the body already fire its own terminal, so the epilogue emitted
    // nothing? (A clean early-abort / `[DONE]` before the post-loop.)
    [[nodiscard]] bool already_terminated() const noexcept {
        return end == StreamEnd::AlreadyTerminated;
    }

    // For the handful of pre-flight bail-outs a transport handles itself
    // (not authenticated / request-build failure) before the stream loop even
    // starts — it emits its own StreamError and returns this so the value still
    // reflects reality.
    static StreamResult failed(std::string msg) {
        StreamResult r;
        r.end   = StreamEnd::TransportError;
        r.error = std::move(msg);
        return r;
    }
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

// Everything a transport's post-loop needs to end a turn, in one bundle. All
// hooks are optional; sensible defaults keep the common case terse. Every fact
// is stated ONCE: `terminated` is a reference to the transport's own latch (not
// a copy passed twice), and `sink` is the transport's EventSink — so
// finish_stream takes exactly this one argument and nothing is duplicated at
// the call site.
struct StreamOutcome {
    bool&              terminated;    // the transport's terminal latch (in/out)
    const EventSink&   sink;          // where the terminal Msg is emitted
    bool               result_ok;     // bool(result) of the http stream call
    int                http_status;   // observed status (0 = headers never came)
    http::CancelTokenPtr cancel;      // caller's cancel token
    StopReason         stop = StopReason::EndTurn;   // stop for CleanClose

    // Build the user-facing error message for an HTTP >= 400 status from the
    // buffered error body. Required when a 4xx/5xx is possible.
    std::function<std::string()>                 http_error_message;
    // Optional Retry-After hint to attach to an HTTP error (429/529).
    std::optional<std::chrono::seconds>          retry_after;
    // Render the transport-level error (connection/TLS/reset). Required.
    std::function<std::string()>                 transport_error_message;
    // Success-only hook: flush held text / close a tool block / salvage a
    // truncated tool call, just before StreamFinished. Runs for CleanClose.
    std::function<void()>                        before_finish;
    // ALL-paths hook, run once before the terminal event on every non-
    // AlreadyTerminated exit (success AND error). The place for cleanup that
    // must happen whether or not the turn succeeded — e.g. Anthropic
    // synthesises a StreamToolUseEnd here so the reducer's salvage path runs
    // on partial tool JSON even when the peer died mid-tool-use.
    std::function<void()>                        on_any_end;
};

// The WHOLE post-loop of a streaming transport, unified. Classify the loop
// exit, then emit exactly one terminal event with the right message/precedence
// — identical for every provider. Replaces each transport's bespoke
// `emit_terminal` lambda + hand-rolled `if (!result) … if (!is_success) …`
// ladder with a single call, so a new transport ends a turn correctly for free:
//
//   return finish_stream({
//       .terminated = ctx.terminated, .sink = ctx.sink,
//       .result_ok = bool(result), .http_status = http_status,
//       .cancel = cancel, .stop = ctx.stop,
//       .http_error_message = [&]{ return build_http_error(); },
//       .transport_error_message = [&]{ return result.error().render(); },
//       .before_finish = [&]{ flush_and_salvage(ctx); },
//   });
//
// `o.terminated` is the transport's own latch (a reference), so a partial body
// that already finished is respected and a later call can't double-finish. On
// UserCancelled we emit StreamError{"cancelled"}; on AlreadyTerminated we emit
// nothing.
//
// Returns a StreamResult describing the outcome. The emitted terminal Msg is
// derived from the SAME classification, so the return value and the sink event
// never disagree — callers that want the outcome read the value, callers that
// react to Msgs still see StreamFinished / StreamError.
inline StreamResult finish_stream(StreamOutcome o) {
    bool& terminated = o.terminated;
    const EventSink& sink = o.sink;
    const StreamEnd end = classify_stream_end(o.terminated, o.result_ok,
                                              o.http_status, o.cancel);
    StreamResult tr;
    tr.end         = end;
    tr.stop        = o.stop;
    tr.http_status = o.http_status;
    tr.retry_after = o.retry_after;

    if (end == StreamEnd::AlreadyTerminated) return tr;
    // All-paths cleanup (e.g. close an open tool block) before the terminal
    // event, on both success and error. Runs exactly once.
    if (o.on_any_end) o.on_any_end();
    switch (end) {
        case StreamEnd::AlreadyTerminated:
            return tr;
        case StreamEnd::UserCancelled:
            tr.error = std::string{"cancelled"};
            finish_turn_once(terminated, sink, o.stop, tr.error);
            return tr;
        case StreamEnd::HttpError:
            tr.error = o.http_error_message
                           ? o.http_error_message()
                           : std::string{"HTTP "} + std::to_string(o.http_status);
            // Stamp the precise status on the Msg so the retry reducer can
            // classify via the typed provider::classify(HttpError) path.
            finish_turn_once(terminated, sink, o.stop, tr.error, o.retry_after,
                             {}, o.http_status);
            return tr;
        case StreamEnd::TransportError:
            tr.error = o.transport_error_message
                           ? o.transport_error_message()
                           : std::string{"transport error"};
            finish_turn_once(terminated, sink, o.stop, tr.error);
            return tr;
        case StreamEnd::CleanClose:
            finish_turn_once(terminated, sink, o.stop, std::nullopt,
                             std::nullopt, o.before_finish);
            return tr;
    }
    return tr;
}

} // namespace agentty::provider
