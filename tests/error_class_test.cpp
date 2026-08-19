// error_class_test — the retry classifier's status-preferring dispatch.
//
// provider::classify_stream_error(message, http_status) is what the runtime's
// StreamError reducer calls to decide auto-retry vs re-auth vs surface. Its
// contract: when the transport stamped a real HTTP status on the Msg
// (http_status != 0), classify via the TYPED classify(HttpError) path — the
// transport already knew the exact status, so no substring sniff of the human
// message. When http_status == 0 (SSE `event: error`, transport/socket
// failure, user cancel, synthetic stall), fall back to the string sniff.
//
// This locks the "status wins over prose" behaviour that lets a proxy phrasing
// "429" as "Too Many Requests" (no digits) still retry, and stops a terminal
// 400 whose message contains "connection" from being mis-retried as transient.

#include <string_view>

#include "agtest.hpp"

#include "agentty/provider/error_class.hpp"

using namespace agentty::provider;

// ── Status-set: the typed path wins, message text is ignored ───────────────
TEST_CASE("status wins over message prose") {
    // 429 with a prose-only body (no digits) → RateLimit via the typed path.
    // The old string sniff would have missed this (no "429"/"rate_limit").
    CHECK(classify_stream_error("Too Many Requests", 429) == ErrorClass::RateLimit);

    // 401/403 → Auth regardless of message wording.
    CHECK(classify_stream_error("Unauthorized", 401) == ErrorClass::Auth);
    CHECK(classify_stream_error("Forbidden", 403) == ErrorClass::Auth);

    // 5xx → Transient.
    CHECK(classify_stream_error("Bad Gateway", 502) == ErrorClass::Transient);
    CHECK(classify_stream_error("Service Unavailable", 503) == ErrorClass::Transient);
    CHECK(classify_stream_error("overloaded_error", 529) == ErrorClass::Transient);

    // A terminal 400 whose message happens to contain a "transient-looking"
    // word ("connection") must NOT be mis-retried — the status is decisive.
    CHECK(classify_stream_error("bad request: connection field invalid", 400)
          == ErrorClass::Terminal);
    CHECK(classify_stream_error("model not found", 404) == ErrorClass::Terminal);
}

// ── Status-zero: fall back to the substring sniff (unchanged behaviour) ─────
TEST_CASE("status-zero falls back to substring sniff") {
    // User cancel: StreamError{"cancelled"} carries no status → Cancelled.
    CHECK(classify_stream_error("cancelled", 0) == ErrorClass::Cancelled);

    // SSE event:error bodies (Anthropic wire text, no status on the Msg).
    CHECK(classify_stream_error("Overloaded", 0) == ErrorClass::Transient);
    CHECK(classify_stream_error("rate_limit_error", 0) == ErrorClass::RateLimit);
    CHECK(classify_stream_error("authentication_error", 0) == ErrorClass::Auth);

    // Transport/socket failures rendered as prose.
    CHECK(classify_stream_error("connection reset", 0) == ErrorClass::Transient);
    CHECK(classify_stream_error("stream stall", 0) == ErrorClass::Transient);

    // Genuinely unknown prose with no status → Terminal.
    CHECK(classify_stream_error("invalid request: missing field", 0)
          == ErrorClass::Terminal);
}

// ── The two paths agree where they overlap ────────────────────────────
// classify_stream_error(msg, status) with a status must equal the direct typed
// call, and with status 0 must equal the direct string call — it's a pure
// dispatcher, no logic of its own.
TEST_CASE("classify_stream_error is a pure dispatcher") {
    using K = agentty::http::HttpErrorKind;
    CHECK(classify_stream_error("whatever", 503)
          == classify(agentty::http::HttpError{K::Status, 503, ""}));
    CHECK(classify_stream_error("Overloaded", 0) == classify("Overloaded"));
}

// ── Long-context entitlement rejection: the [1m] self-heal trigger ───────
// Anthropic 400s the whole request when the context-1m beta rides on an
// unentitled subscription. The reducer strips the `[1m]` marker and retries;
// this detector is its gate, so lock its shape: the real message matches (any
// casing / suffix rewording), unrelated 400s and non-400s don't.
TEST_CASE("is_long_context_rejection") {
    using agentty::provider::is_long_context_rejection;
    // The exact wire message observed in production.
    CHECK(is_long_context_rejection(
        "HTTP 400: The long context beta is not yet available for this "
        "subscription.", 400));
    // SSE event:error path carries no status (0) — must still match.
    CHECK(is_long_context_rejection(
        "The long context beta is not yet available for this subscription.", 0));
    // Upstream rewording of the tail keeps matching (stable prefix).
    CHECK(is_long_context_rejection(
        "the Long Context Beta is not enabled for this organization", 400));
    // Future drift naming the beta id directly.
    CHECK(is_long_context_rejection(
        "unsupported beta: context-1m-2025-08-07", 400));
    // Unrelated 400s must NOT trigger the fallback.
    CHECK(!is_long_context_rejection("invalid request: missing field", 400));
    CHECK(!is_long_context_rejection("prompt is too long", 400));
    // The right words on the wrong status must NOT trigger it either
    // (a 529/503 mentioning "long context" in prose is not the beta gate).
    CHECK(!is_long_context_rejection(
        "long context beta is not available", 503));
}
