#pragma once
// agentty::provider::copilot — native GitHub Copilot OAuth + token exchange.
//
// Copilot's chat API is OpenAI-Chat-compatible, so inference reuses the generic
// openai transport. The only Copilot-specific work is AUTH, a two-stage flow:
//
//   1. GitHub DEVICE FLOW → a long-lived GitHub user token (ghu_…), persisted
//      once. (Plain device flow — no PKCE, no loopback server, no browser
//      redirect. The user types a code at github.com/login/device.)
//   2. TOKEN EXCHANGE → GET api.github.com/copilot_internal/v2/token with the
//      ghu_ token → a SHORT-LIVED Copilot proxy token (~30 min TTL) plus the
//      per-account inference host (`endpoints.api`). Auto-refreshed in-process
//      before it expires, with clock-skew safety and cross-process de-dup.
//
// This mirrors the ChatGPT/Codex OAuth module's shape; where Codex uses PKCE +
// a refresh_token, Copilot uses the durable ghu_ token as the refresh source.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "agentty/auth/auth.hpp"   // OAuthError

namespace agentty::provider::copilot {

using auth::OAuthError;

// The durable GitHub credential from the device flow. This is what `agentty
// login → GitHub Copilot` persists; it does not expire on its own (revocation
// forces a re-login).
struct GithubToken {
    std::string access_token;   // ghu_… (or gho_… for legacy); the exchange source
    std::string token_type;     // "bearer"
    std::string scope;          // granted scopes (informational)
};

// The short-lived Copilot proxy token from the exchange. Cached alongside the
// GithubToken and refreshed transparently.
struct CopilotToken {
    std::string token;              // send as `Authorization: Bearer <token>`
    std::int64_t expires_at_ms = 0; // skew-safe LOCAL expiry (now + refresh_in + slack)
    std::string endpoint_api;       // inference base URL from `endpoints.api`
    std::string sku;                // e.g. "free_limited_copilot"
    bool chat_enabled = true;       // false ⇒ account has no chat entitlement
    bool quota_exhausted = false;   // free-tier chat quota hit

    [[nodiscard]] bool expired(std::int64_t skew_ms = 0) const noexcept {
        return expires_at_ms != 0 && now_ms() >= expires_at_ms - skew_ms;
    }
    [[nodiscard]] bool valid() const noexcept { return !token.empty(); }

    static std::int64_t now_ms() noexcept;
};

// ── Device-flow login ────────────────────────────────────────────────────
// The UI seam: agentty shows this to the user ("go to <url> and enter <code>").
struct DeviceCode {
    std::string verification_uri;   // https://github.com/login/device
    std::string user_code;          // e.g. "31B0-2B8C"
    int         expires_in = 900;
};
using DeviceCodeSink = std::function<void(const DeviceCode&)>;
using CancelProbe    = std::function<bool()>;

// Run the full device flow: request a code, invoke `on_device_code` so the host
// can display it (+ open the browser), then poll until the user authorises.
// Persists the resulting GithubToken on success. Blocking; honours `cancelled`.
[[nodiscard]] std::expected<GithubToken, OAuthError>
login(int timeout_s, DeviceCodeSink on_device_code, CancelProbe cancelled);

// ── Persistence ──────────────────────────────────────────────────────────
[[nodiscard]] std::optional<GithubToken> load_github_token();
bool save_github_token(const GithubToken&);
bool clear_credentials();                 // wipes both the ghu_ token and cache
[[nodiscard]] std::filesystem::path credentials_path();

// True when a GitHub token is on disk (i.e. the user has signed in). Cheap; no
// network. The proxy token may still need a first exchange.
[[nodiscard]] bool signed_in();

// ── Token exchange / refresh ─────────────────────────────────────────────
// Return a VALID Copilot proxy token, exchanging or refreshing from the stored
// ghu_ token as needed. Single-flight + cross-process de-duplicated. nullopt
// when not signed in or the exchange fails (chat disabled, network, revoked).
[[nodiscard]] std::optional<CopilotToken> fresh_token();

// Force a re-exchange on the next fresh_token() (called after a 401 from the
// inference endpoint, in case the cached proxy token was revoked early).
void invalidate_cached_token();

// ── Pure helpers (exposed for tests) ─────────────────────────────────────
// Parse the `/copilot_internal/v2/token` success JSON into a CopilotToken,
// applying the skew-safe LOCAL expiry (prefer refresh_in + slack; fall back to
// expires_at; else a conservative 25-min default) and resolving the inference
// host from `endpoints.api`. `now_ms` is injectable so the expiry math is
// deterministic under test. Returns nullopt when the JSON has no `token`.
[[nodiscard]] std::optional<CopilotToken>
parse_token_envelope(std::string_view json_body, std::int64_t now_ms);

} // namespace agentty::provider::copilot
