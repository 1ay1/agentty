# Authentication

moha supports two authentication methods for the Claude API: **OAuth** (Claude.ai login) and **API key**. OAuth reuses Claude Code's OAuth client, so users with a Pro/Max subscription can authenticate without separate API billing.

## Quick Start

```bash
moha login          # Interactive login (OAuth or API key)
moha status         # Show current auth status
moha logout         # Remove saved credentials
moha -k sk-ant-... "hello"  # One-off API key override
```

## Authentication Methods

### OAuth (Claude.ai Login)

Uses OAuth 2.0 Authorization Code with PKCE. The token is sent as `Authorization: Bearer <token>` and enables Pro/Max subscription billing.

**OAuth client config** (`src/core/auth.hpp:20-28`):

| Field | Value |
|---|---|
| Client ID | `9d1c250a-e61b-44d9-88ed-5944d1962f5e` |
| Authorize URL | `https://claude.ai/oauth/authorize` |
| Token URL | `https://platform.claude.com/v1/oauth/token` |
| Callback URL | `https://platform.claude.com/oauth/code/callback` |
| Scopes | `user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload` |

This is the same OAuth client as Claude Code, which enables token reuse via the `CLAUDE_CODE_OAUTH_TOKEN` environment variable.

### API Key

Standard Anthropic API keys (`sk-ant-...`). Sent as `x-api-key` header. Uses API billing.

## Credential Resolution

On startup, credentials are resolved in this priority order (`src/main.cpp:193-217`):

1. **CLI flag** `-k` / `--key` — API key override for a single session
2. **`ANTHROPIC_API_KEY`** env var — API key
3. **`CLAUDE_CODE_OAUTH_TOKEN`** env var — OAuth token (reuse Claude Code's auth)
4. **Credentials file** — `~/.config/moha/credentials.json` (or `$XDG_CONFIG_HOME/moha/credentials.json`)

If an OAuth token is expired and a refresh token is available, moha automatically refreshes before starting.

## Credentials File

**Path:** `~/.config/moha/credentials.json` (permissions: `0600`)

```json
{
  "method": "oauth",
  "access_token": "<token>",
  "refresh_token": "<refresh_token>",
  "expires_at": 1712345678000
}
```

The `method` field is `"oauth"` for OAuth tokens or `"api_key"` for API keys. `expires_at` is Unix milliseconds; `0` means no expiration info. API keys never expire.

## OAuth PKCE Flow

The interactive login (`moha login` → option 1) implements Authorization Code with PKCE (`src/core/auth.cpp:364-441`):

```
┌─ moha ────────────────────────────────────────────────────┐
│                                                           │
│  1. Generate 128-char code_verifier (random charset)      │
│  2. code_challenge = base64url(SHA-256(code_verifier))    │
│  3. Build authorize URL with PKCE params                  │
│  4. Open browser (xdg-open / open) or print URL           │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │
                            ▼
┌─ Browser ─────────────────────────────────────────────────┐
│                                                           │
│  User logs in at claude.ai/oauth/authorize                │
│  Redirected to platform.claude.com/oauth/code/callback    │
│  Page displays authorization code                         │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │ user pastes code
                            ▼
┌─ moha ────────────────────────────────────────────────────┐
│                                                           │
│  5. POST to platform.claude.com/v1/oauth/token:           │
│     { grant_type: "authorization_code",                   │
│       code: "<pasted_code>",                              │
│       client_id: "9d1c250a-...",                          │
│       code_verifier: "<128-char verifier>",               │
│       redirect_uri: "https://platform.claude.com/...",    │
│       state: "<32-char random>" }                         │
│                                                           │
│  6. Receive: { access_token, refresh_token, expires_in }  │
│  7. Save to ~/.config/moha/credentials.json (0600)        │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

## Token Refresh

OAuth tokens expire (typically after 1 hour). moha checks expiration on startup and refreshes automatically if a refresh token is available (`src/core/auth.cpp:321-349`):

```
POST https://platform.claude.com/v1/oauth/token
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token
&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e
&refresh_token=<saved_refresh_token>
```

The response provides a new `access_token` (and optionally a new `refresh_token`), which is saved to disk.

## How Auth Headers Are Attached

In `agent_loop::stream_response()` (`src/core/agent_loop.cpp:126-146`):

```cpp
if (config_.auth == auth_style::bearer) {
    // OAuth: Authorization header
    headers.emplace_back("Authorization", config_.api_key);
    // Bearer auth enables the oauth beta
    headers.emplace_back("anthropic-beta",
        "oauth-2025-04-20,prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
} else {
    // API key: x-api-key header
    headers.emplace_back("x-api-key", config_.api_key);
    headers.emplace_back("anthropic-beta",
        "prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
}
```

Note: `config_.api_key` already contains `"Bearer <token>"` for OAuth (set by `credentials::header_value()` → `main.cpp:257`), or the raw key for API key auth.

### OAuth System Prompt Billing Header

When using OAuth, the system prompt includes a billing tracking block (`agent_loop.cpp:23-37`):

```json
[
  { "type": "text", "text": "x-anthropic-billing-header: cc_version=0.1.0; cc_entrypoint=cli; cch=00000;" },
  { "type": "text", "text": "<actual system prompt>", "cache_control": { "type": "ephemeral" } }
]
```

This enables Pro/Max subscription billing for the request.

## Error Handling

Authentication failures (HTTP 401/403) are **not retried**. The user must re-login (`agent_loop.cpp:253-256`):

```
Authentication failed (HTTP 401). Run 'moha login' to re-authenticate, or check your API key.
```

Rate limits (429) and overload (529) are retried with exponential backoff (1s, 2s, 4s, 8s, max 30s, up to 4 retries).

## Data Structures

### `credentials` (`src/core/auth.hpp:32-41`)

```cpp
struct credentials {
    auth_method method = auth_method::none;  // none | api_key | oauth_token
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at = 0;  // unix milliseconds

    bool is_valid() const;    // access_token not empty
    bool is_expired() const;  // only for oauth; checks now_ms() >= expires_at
    std::string header_value() const;  // "Bearer <token>" or raw key
};
```

### `agent_config` (`src/core/agent_loop.hpp:32-41`)

```cpp
struct agent_config {
    std::string api_key;       // raw key or "Bearer <token>"
    auth_style auth = auth_style::api_key;  // api_key | bearer
    std::string api_url = "https://api.anthropic.com/v1/messages";
    std::string model = "claude-sonnet-4-20250514";
    std::string system_prompt;
    int max_tokens = 16384;
    profile prof = profile::write;
};
```

## Full Flow: Startup to Authenticated API Call

```
main()
  ├─ parse_args()             — extract --key, --model, subcommand
  ├─ subcommand?
  │   ├─ "login"  → cmd_login()  → login_interactive()
  │   ├─ "logout" → cmd_logout() → clear_credentials()
  │   └─ "status" → cmd_status() → display token info
  │
  ├─ resolve_credentials()
  │   ├─ --key flag?              → use as API key
  │   ├─ ANTHROPIC_API_KEY?       → use as API key
  │   ├─ CLAUDE_CODE_OAUTH_TOKEN? → use as OAuth token
  │   ├─ credentials.json?        → load from disk
  │   │   └─ expired + has refresh_token? → refresh_access_token()
  │   └─ nothing found            → error, prompt "moha login"
  │
  ├─ Build agent_config
  │   ├─ OAuth:   config.api_key = "Bearer <token>", auth = bearer
  │   └─ API key: config.api_key = "<key>",          auth = api_key
  │
  ├─ SharedState { agent_loop(config, tools), ... }
  │
  └─ maya::run<App>()  — TUI event loop
       │
       └─ user submits message → run_agent_cmd()
            │
            └─ agent.run_turn(message)
                 │
                 └─ stream_response()
                      ├─ Build headers based on auth style
                      ├─ http_.post_streaming("api.anthropic.com/v1/messages", ...)
                      └─ Stream SSE events back to UI
```

## File Index

| File | Purpose |
|---|---|
| `src/core/auth.hpp` | Types: `auth_method`, `credentials`, `oauth_config` |
| `src/core/auth.cpp` | All auth logic: load/save/clear creds, PKCE, token exchange, refresh, login flows, subcommands |
| `src/core/agent_loop.hpp` | `auth_style` enum, `agent_config` struct |
| `src/core/agent_loop.cpp` | Attaches auth headers, builds system prompt with billing header |
| `src/main.cpp` | CLI arg parsing, credential resolution, wiring config to agent |
| `src/net/http_client.cpp` | Raw HTTP/TLS client used for API calls and token exchange |
