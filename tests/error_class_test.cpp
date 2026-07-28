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

#include <cstdio>
#include <string_view>

#include "agentty/provider/error_class.hpp"

using namespace agentty::provider;

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d  %s\n",                     \
                         __FILE__, __LINE__, #cond);                     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

// ── Status-set: the typed path wins, message text is ignored ────────────────
static void test_status_beats_message() {
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
static void test_no_status_falls_back_to_string() {
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

// ── The two paths agree where they overlap ──────────────────────────────────
// classify_stream_error(msg, status) with a status must equal the direct typed
// call, and with status 0 must equal the direct string call — it's a pure
// dispatcher, no logic of its own.
static void test_dispatcher_is_pure() {
    using K = agentty::http::HttpErrorKind;
    CHECK(classify_stream_error("whatever", 503)
          == classify(agentty::http::HttpError{K::Status, 503, ""}));
    CHECK(classify_stream_error("Overloaded", 0) == classify("Overloaded"));
}

int main() {
    test_status_beats_message();
    test_no_status_falls_back_to_string();
    test_dispatcher_is_pure();

    if (g_failures == 0) {
        std::printf("error_class_test: all checks passed\n");
        return 0;
    }
    std::printf("error_class_test: %d failure(s)\n", g_failures);
    return 1;
}
