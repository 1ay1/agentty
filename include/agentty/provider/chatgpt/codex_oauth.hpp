#pragma once
// agentty::provider::chatgpt — native ChatGPT (Codex) OAuth.
//
// This is the "log in with ChatGPT, just like Claude" path: agentty runs the
// Authorization-Code + PKCE (S256) flow against auth.openai.com itself, mints
// the tokens (and, via RFC-8693 token-exchange, a usable OpenAI API key),
// and stores them under its OWN encrypted credential file — no dependency on
// the `codex` binary and no touching of ~/.codex secrets.
//
// It reuses the shared PKCE / HTTP / crypto helpers in `agentty::auth`; only
// the OpenAI-specific wiring (endpoints, the loopback callback server, the
// api-key exchange, the separate token store) lives here.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/chatgpt/oauth.hpp"

namespace agentty::provider::chatgpt {

// The persisted ChatGPT credential. `api_key` is the OpenAI key minted from
// the id_token — preferred for API calls; `access_token` is the raw OAuth
// bearer (used against chatgpt.com/backend-api/codex with `account_id`).
struct CodexCredentials {
    std::string  access_token;
    std::string  refresh_token;
    std::string  id_token;
    std::string  account_id;   // chatgpt_account_id claim from the id_token
    std::string  api_key;      // minted OpenAI key (may be empty)
    std::int64_t expires_at_ms = 0;

    [[nodiscard]] bool empty() const noexcept { return access_token.empty(); }
    [[nodiscard]] bool expired(std::int64_t skew_ms = 0) const noexcept;
};

// ── Token store (separate from the Anthropic credentials.json) ─────────────
[[nodiscard]] std::filesystem::path codex_credentials_path();
[[nodiscard]] std::optional<CodexCredentials> load_codex_credentials();
bool save_codex_credentials(const CodexCredentials& c);
bool clear_codex_credentials();

// ── Authorize URL ──────────────────────────────────────────────────────────
// Pure: same (verifier, state) → same URL. Points at the loopback redirect.
[[nodiscard]] std::string codex_authorize_url(const auth::PkceVerifier& verifier,
                                              const auth::OAuthState&   state);

// ── Interactive login ──────────────────────────────────────────────────────
// SSH sessions automatically use OpenAI's device-code flow: agentty displays
// a URL + one-time code and polls until the browser on any machine approves
// it. Local sessions retain the loopback callback flow on port 1455.
struct CodexDeviceCode {
    std::string verification_url;
    std::string user_code;
};

using CodexDeviceCodeSink = std::function<void(const CodexDeviceCode&)>;
using CodexCancelProbe = std::function<bool()>;

// True when the environment requests device auth explicitly or identifies an
// SSH session. AGENTTY_CHATGPT_DEVICE_AUTH=0/1 is an override for unusual
// terminals and test harnesses.
[[nodiscard]] bool codex_device_auth_preferred() noexcept;

// Runs the appropriate full flow and returns credentials without persisting
// them. The caller must save only after it confirms the attempt is still
// active. `on_device_code` fires once before polling begins.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_login(int timeout_s = 900, CodexDeviceCodeSink on_device_code = {},
            CodexCancelProbe cancelled = {});

// Explicit device flow with the same side-effect-free completion contract.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_device_login(CodexDeviceCodeSink on_device_code, int timeout_s = 900,
                   CodexCancelProbe cancelled = {});

// ── Exchange / refresh / mint (exposed for the reducer + tests) ────────────
// Exchange an authorization code (from the callback) for tokens.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_exchange_code(const auth::OAuthCode& code, const auth::PkceVerifier& verifier);

// Refresh an expired access token via the refresh_token (JSON body, per the
// Codex protocol). Returns a fresh credential; re-mints the API key.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_refresh(const CodexCredentials& current);

// Return a valid credential, refreshing in place (and persisting) if the
// stored access token is stale. Worker-thread safe. Empty optional when no
// ChatGPT credential is saved.
[[nodiscard]] std::optional<CodexCredentials> codex_fresh_credentials();

} // namespace agentty::provider::chatgpt
