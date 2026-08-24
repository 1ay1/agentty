// kimi_token_test — the pure Kimi Code OAuth parsing / expiry logic (no network).
//
// Locks the token-response parser and the skew-safe refresh decision, the
// load-bearing, easy-to-get-subtly-wrong parts of the Kimi integration (wrong
// expiry math → mid-turn 401s; missing refresh_token on refresh → surprise
// logout). Kimi's device-flow access_token is the API bearer directly (no
// proxy exchange), refreshed via the refresh_token grant.

#include <string>

#include "agtest.hpp"

#include "agentty/provider/kimi/kimi_oauth.hpp"

using namespace agentty;
using namespace agentty::provider::kimi;

TEST_CASE("kimi token response parse + expiry") {
    constexpr std::int64_t kNow = 1'000'000'000'000;   // fixed "now" in ms

    // ── a full token bundle parses; expiry = now + (expires_in - 60s) ───────
    {
        auto t = parse_token_response(
            R"({"access_token":"at-123","refresh_token":"rt-456",
                "token_type":"Bearer","scope":"api","expires_in":3600})", kNow);
        CHECK(t.has_value(), "valid response parses");
        CHECK(t->access_token == "at-123", "access_token extracted");
        CHECK(t->refresh_token == "rt-456", "refresh_token extracted");
        CHECK(t->token_type == "Bearer", "token_type extracted");
        CHECK(t->scope == "api", "scope extracted");
        // expires_in (3600) → now + (3600-60)*1000 (skew slack).
        CHECK(t->expires_at_ms == kNow + (3600 - 60) * 1000,
              "expiry = now + (expires_in - slack) — refresh before real expiry");
    }

    // ── no expires_in → expiry left at 0 (unknown, treated as non-expiring) ─
    {
        auto t = parse_token_response(
            R"({"access_token":"x","refresh_token":"y"})", kNow);
        CHECK(t && t->expires_at_ms == 0, "no expires_in → 0 (unknown)");
        CHECK(!t->expired(999999), "unknown expiry (0) is never treated as expired");
    }

    // ── malformed / empty → nullopt, never a throw ──────────────────────────
    CHECK(!parse_token_response("not json", kNow), "garbage → nullopt");
    CHECK(!parse_token_response("{}", kNow), "no access_token field → nullopt");
    CHECK(!parse_token_response(R"({"access_token":""})", kNow),
          "empty access_token → nullopt");

    // ── expired() is skew-aware (uses the real wall clock) ──────────────────
    {
        KimiToken past;
        past.access_token = "x";
        past.expires_at_ms = KimiToken::now_ms() - 1000;
        CHECK(past.expired(0), "a past expiry is expired");

        KimiToken future;
        future.access_token = "x";
        future.expires_at_ms = KimiToken::now_ms() + 30 * 60 * 1000;   // 30 min out
        CHECK(!future.expired(0), "a 30-min-out token is not expired");

        KimiToken soon;
        soon.access_token = "x";
        soon.expires_at_ms = KimiToken::now_ms() + 20 * 1000;   // 20 s out
        CHECK(soon.expired(30 * 1000),
              "a token inside the skew buffer is treated as stale (refresh early)");
        CHECK(!soon.expired(0), "…but not stale without the buffer");

        KimiToken never;
        never.access_token = "x";
        never.expires_at_ms = 0;   // 0 = unknown
        CHECK(!never.expired(999999), "unknown expiry (0) is never treated as expired");
    }

    // ── valid() gates on a non-empty access token ───────────────────────────
    {
        KimiToken empty;
        CHECK(!empty.valid(), "empty token is not valid");
        KimiToken ok;
        ok.access_token = "x";
        CHECK(ok.valid(), "non-empty access_token is valid");
    }
}
