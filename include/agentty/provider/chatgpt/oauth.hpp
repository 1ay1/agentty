#pragma once
// Codex (ChatGPT) OAuth wiring. Every constant here identifies agentty to
// OpenAI's auth edge and mirrors the Codex CLI's own login flow — changing any
// of them breaks the flow, so they live in one provider-scoped place instead
// of leaking into `agentty::auth`.
//
// Reversed from codex-rs/login/src (auth/manager.rs, server.rs, pkce.rs) and
// the installed `codex` binary. The flow is Authorization-Code + PKCE (S256)
// with a FIXED loopback redirect on port 1455 — the same shape as Claude
// Code's OAuth, but OpenAI requires the code to come back to a local HTTP
// listener rather than a hosted paste-the-code page.

namespace agentty::provider::chatgpt {

struct OAuthConfig {
    // Codex CLI's public OAuth client id. Using it is what lets agentty ride a
    // user's existing ChatGPT (Codex) session, exactly as the Anthropic path
    // rides a Claude.ai session.
    static constexpr const char* client_id     = "app_EMoamEEZ73f0CkXaXp7hrann";
    static constexpr const char* issuer        = "https://auth.openai.com";
    static constexpr const char* authorize_url = "https://auth.openai.com/oauth/authorize";
    static constexpr const char* token_url     = "https://auth.openai.com/oauth/token";

    // OpenAI pins the loopback redirect to this exact host+port+path. The
    // local callback server MUST bind 1455 and serve /auth/callback.
    static constexpr const char* redirect_uri  = "http://localhost:1455/auth/callback";
    static constexpr int         callback_port = 1455;
    static constexpr const char* callback_path = "/auth/callback";

    static constexpr const char* scopes =
        "openid profile email offline_access "
        "api.connectors.read api.connectors.invoke";

    // Sent verbatim as an authorize query param so OpenAI treats us like the
    // Codex CLI (drives the simplified consent screen + org-in-id_token).
    static constexpr const char* originator = "codex_cli_rs";

    // The Codex CLI version agentty impersonates on Codex backend requests.
    // CRITICAL: the /backend-api/codex/models catalog endpoint gates the
    // returned model list on `client_version` — an old/unknown version gets a
    // 200 with an EMPTY `{"models":[]}` (the server withholds the catalog from
    // stale clients). agentty's own version (0.2.x) is far below any codex
    // minimum, so we MUST send a current codex version here (both the
    // client_version query param and the codex_cli_rs/<ver> user-agent) or the
    // picker falls back to a bundled guess and the first turn 400s on a slug
    // the account no longer offers. Bump this when the catalog empties again.
    static constexpr const char* codex_client_version = "0.145.0";

    // RFC-8693 token-exchange used to mint a usable OpenAI API key from the
    // signed id_token — this is what lets agentty call the API directly
    // WITHOUT the codex binary present.
    static constexpr const char* token_exchange_grant =
        "urn:ietf:params:oauth:grant-type:token-exchange";
    static constexpr const char* requested_token_apikey = "openai-api-key";
    static constexpr const char* subject_token_type_idtoken =
        "urn:ietf:params:oauth:token-type:id_token";
};

} // namespace agentty::provider::chatgpt
