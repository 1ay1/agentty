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
// Runs the full flow: spins up the loopback server on port 1455, opens the
// browser, waits for the callback, exchanges the code, mints the API key, and
// persists the result. Blocking (bounded by `timeout_s`); intended for
// `cmd_login` and a Cmd::task off the reducer. Returns the stored credential
// or a typed OAuthError.
[[nodiscard]] std::expected<CodexCredentials, auth::OAuthError>
codex_login(int timeout_s = 300);

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
