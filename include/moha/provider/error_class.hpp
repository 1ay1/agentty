#pragma once
// moha::provider::error_class — classify a stream-level error message
// from the wire so the reducer knows whether to retry, prompt for
// re-auth, or surface as a terminal failure.
//
// Anthropic surfaces errors via SSE `event: error` carrying JSON like:
//   {"type":"error","error":{"type":"overloaded_error","message":"…"}}
// Our transport extracts `error.message` (plus a status-code prefix
// like "http: 429: …" for HTTP-layer failures). This module pattern-
// matches against both shapes by simple substring sniffing — no JSON
// re-parse, no schema dependency. False negatives stay terminal
// (safe); false positives just retry once and then settle.

#include <string>
#include <string_view>

namespace moha::provider {

enum class ErrorClass {
    // Transient — retryable with backoff. Server is up but momentarily
    // unhappy (load shed, queue full, 5xx). Same request will likely
    // succeed in a few seconds.
    Transient,
    // Rate-limited — retryable with longer backoff. Often carries a
    // Retry-After hint upstream; we use a flat schedule here.
    RateLimit,
    // Authentication — the OAuth token expired mid-session or was
    // revoked. Caller should refresh and retry once; if refresh fails,
    // surface as terminal so user can `moha login`.
    Auth,
    // Cancelled — user pressed Esc; never retry. Final.
    Cancelled,
    // Terminal — invalid request, model not found, billing, etc.
    // Re-sending will fail the same way. Surface to the user and stop.
    Terminal,
};

// Lower-case substring sniff. `msg` is the error message as surfaced by
// the transport — it may be wrapped ("http: 503: …", "request build
// failed: …") or be the raw `error.message` from an SSE error event
// ("Overloaded", "rate_limit_error", etc.). Uses byte-wise compare
// against ASCII tokens, so locale doesn't matter.
[[nodiscard]] inline ErrorClass classify(std::string_view msg) noexcept {
    auto contains = [&](std::string_view needle) noexcept -> bool {
        if (needle.size() > msg.size()) return false;
        for (std::size_t i = 0; i + needle.size() <= msg.size(); ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                char a = msg[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };

    // Cancellation comes through as a dedicated string from the worker.
    if (contains("cancel")) return ErrorClass::Cancelled;

    // Auth surfaces as HTTP 401/403 from the wire layer or as the
    // explicit "authentication_error" type from Anthropic's body.
    if (contains("401")
     || contains("403")
     || contains("authentication_error")
     || contains("invalid api key")
     || contains("not authenticated"))
        return ErrorClass::Auth;

    // Rate limit — Anthropic's "rate_limit_error" or HTTP 429.
    if (contains("rate_limit") || contains("429"))
        return ErrorClass::RateLimit;

    // Overload / server / network — transient.
    if (contains("overloaded")
     || contains("overload_error")
     || contains("502")
     || contains("503")
     || contains("504")
     || contains("529")              // Anthropic's "overloaded" HTTP code
     || contains("connection")       // "connection refused/reset"
     || contains("timeout")
     || contains("eof")
     || contains("broken pipe")
     || contains("network"))
        return ErrorClass::Transient;

    return ErrorClass::Terminal;
}

// Backoff in milliseconds for the Nth retry attempt (0-indexed). Caps
// at 5 attempts; longer schedules for RateLimit since Anthropic's
// per-minute window doesn't reset on demand.
[[nodiscard]] inline int backoff_ms(ErrorClass kind, int attempt) noexcept {
    if (attempt < 0) attempt = 0;
    if (attempt > 4) attempt = 4;
    if (kind == ErrorClass::RateLimit) {
        static constexpr int ms[5] = {3000, 8000, 20000, 40000, 60000};
        return ms[attempt];
    }
    // Transient / Auth retry — fast first, then linger.
    static constexpr int ms[5] = {1000, 3000, 7000, 15000, 30000};
    return ms[attempt];
}

// Hard cap on automatic retries. Past this, surface as terminal.
inline constexpr int kMaxRetries = 4;

[[nodiscard]] constexpr std::string_view to_string(ErrorClass k) noexcept {
    switch (k) {
        case ErrorClass::Transient: return "transient";
        case ErrorClass::RateLimit: return "rate_limit";
        case ErrorClass::Auth:      return "auth";
        case ErrorClass::Cancelled: return "cancelled";
        case ErrorClass::Terminal:  return "terminal";
    }
    return "unknown";
}

} // namespace moha::provider
