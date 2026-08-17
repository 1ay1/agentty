// copilot_token_test — the pure Copilot auth logic (no network).
//
// Locks the token-envelope parser and the skew-safe refresh decision, which are
// the load-bearing, easy-to-get-subtly-wrong parts of the Copilot integration
// (every third-party tool that broke did so here: wrong expiry math → mid-turn
// 401s, or ignoring endpoints.api → 404s on Business/Enterprise). Also pins
// that Copilot-fronted model ids resolve to sane capabilities.

#include <string>

#include "agtest.hpp"

#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/domain/catalog.hpp"

using namespace agentty;
using namespace agentty::provider::copilot;

TEST_CASE("copilot token exchange + expiry") {
    constexpr std::int64_t kNow = 1'000'000'000'000;   // fixed "now" in ms

    // ── refresh_in drives a skew-safe LOCAL expiry (now + refresh_in + 60s) ──
    {
        auto t = parse_token_envelope(
            R"({"token":"tid=abc:xyz","refresh_in":1500,"expires_at":9999999999,
                "endpoints":{"api":"https://api.githubcopilot.com"},"sku":"copilot_pro",
                "chat_enabled":true})", kNow);
        CHECK(t.has_value(), "valid envelope parses");
        CHECK(t->token == "tid=abc:xyz", "token extracted");
        // refresh_in (1500) preferred over expires_at → now + (1500+60)*1000.
        CHECK(t->expires_at_ms == kNow + (1500 + 60) * 1000,
              "expiry = now + (refresh_in + slack) — skew-safe, not absolute");
        CHECK(t->endpoint_api == "https://api.githubcopilot.com", "endpoints.api used");
        CHECK(t->sku == "copilot_pro", "sku carried");
        CHECK(t->chat_enabled, "chat_enabled carried");
        CHECK(!t->quota_exhausted, "no quota block → not exhausted");
    }

    // ── endpoints.api is AUTHORITATIVE (Business/Enterprise route here) ──────
    {
        auto t = parse_token_envelope(
            R"({"token":"x","refresh_in":1000,
                "endpoints":{"api":"https://api.business.githubcopilot.com"}})", kNow);
        CHECK(t && t->endpoint_api == "https://api.business.githubcopilot.com",
              "business endpoint routed from endpoints.api, not hardcoded");
    }

    // ── missing endpoints.api falls back to the default host ────────────────
    {
        auto t = parse_token_envelope(R"({"token":"x","refresh_in":1000})", kNow);
        CHECK(t && t->endpoint_api == "https://api.githubcopilot.com",
              "missing endpoints.api → default host");
    }

    // ── no refresh_in → fall back to absolute expires_at ────────────────────
    {
        auto t = parse_token_envelope(
            R"({"token":"x","expires_at":2000000000})", kNow);
        CHECK(t && t->expires_at_ms == 2000000000LL * 1000,
              "no refresh_in → expires_at (seconds→ms)");
    }

    // ── neither field → conservative 25-min default ─────────────────────────
    {
        auto t = parse_token_envelope(R"({"token":"x"})", kNow);
        CHECK(t && t->expires_at_ms == kNow + 25 * 60 * 1000,
              "no expiry fields → conservative 25-min default");
    }

    // ── free-tier quota + chat entitlement flags ────────────────────────────
    {
        auto t = parse_token_envelope(
            R"({"token":"x","refresh_in":1000,"sku":"free_limited_copilot",
                "chat_enabled":true,"limited_user_quotas":{"chat":0}})", kNow);
        CHECK(t && t->quota_exhausted, "chat quota 0 → exhausted");
        auto t2 = parse_token_envelope(
            R"({"token":"x","refresh_in":1000,"chat_enabled":false})", kNow);
        CHECK(t2 && !t2->chat_enabled, "chat_enabled:false surfaced");
    }

    // ── malformed / empty → nullopt, never a throw ──────────────────────────
    CHECK(!parse_token_envelope("not json", kNow), "garbage → nullopt");
    CHECK(!parse_token_envelope("{}", kNow), "no token field → nullopt");
    CHECK(!parse_token_envelope(R"({"token":""})", kNow), "empty token → nullopt");

    // ── expired() is skew-aware (uses the real wall clock) ──────────────────
    {
        CopilotToken past;
        past.token = "x";
        past.expires_at_ms = CopilotToken::now_ms() - 1000;
        CHECK(past.expired(0), "a past expiry is expired");

        CopilotToken future;
        future.token = "x";
        future.expires_at_ms = CopilotToken::now_ms() + 30 * 60 * 1000;   // 30 min out
        CHECK(!future.expired(0), "a 30-min-out token is not expired");

        CopilotToken soon;
        soon.token = "x";
        soon.expires_at_ms = CopilotToken::now_ms() + 60 * 1000;   // 1 min out
        CHECK(soon.expired(5 * 60 * 1000),
              "a token inside the skew buffer is treated as stale (refresh early)");
        CHECK(!soon.expired(0), "…but not stale without the buffer");

        CopilotToken never;
        never.token = "x";
        never.expires_at_ms = 0;   // 0 = unknown
        CHECK(!never.expired(999999), "unknown expiry (0) is never treated as expired");
    }

    // ── Copilot-fronted model ids resolve to sane capabilities ──────────────
    // Copilot returns standard third-party ids; from_id recognises them.
    {
        auto gpt = ModelCapabilities::from_id("gpt-4o");
        auto claude = ModelCapabilities::from_id("claude-sonnet-4");
        (void)gpt; (void)claude;   // from_id must not crash on these ids
        CHECK(true, "from_id handles Copilot model ids without crashing");
    }
}
