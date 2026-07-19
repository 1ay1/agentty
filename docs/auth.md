# Authentication

agentty supports two authentication methods for the Claude API: **OAuth** (Claude.ai login) and **API key**. OAuth reuses Claude Code's OAuth client, so users with a Pro/Max subscription can authenticate without separate API billing.

## Quick Start

```bash
agentty login          # Interactive login (OAuth or API key)
agentty status         # Show current auth status
agentty logout         # Remove saved credentials
agentty -k sk-ant-...  # One-off API-key override for this session
```

CLI flags:

| Flag | Effect |
|---|---|
| `-k`, `--key KEY` | API-key override for this session |
| `-m`, `--model ID` | Model id (e.g. `claude-opus-4-5`) |
| `--auth-header NAME` | Custom auth header name for OpenAI-compatible backends (see below) |
| `-h`, `--help` | Print usage |

Subcommand dispatch lives in `src/main.cpp:1400-1435`.

## Custom Providers (`--auth-header`)

OpenAI-compatible backends (`--provider openai | groq | … | host[:port]`)
authenticate with `Authorization: Bearer <key>` by default. Some self-hosted
or enterprise gateways expect the key under a different header name instead
(e.g. `X-API-Key`). `--auth-header NAME` overrides the header name for the
session; the key (from `-k`, the in-app paste, or `OPENAI_API_KEY` /
provider-specific env var) is sent **raw** under that name — no `Bearer `
prefix:

```bash
agentty --provider my-gateway.lan:9000 --auth-header X-API-Key -k sk-xxx
# emits:  x-api-key: sk-xxx     (instead of authorization: Bearer sk-xxx)
```

Session-scoped like `-k` (not persisted). Applies to every OpenAI-family
request — chat completions, model listing, the Ollama capability probe —
and survives live provider switches (`^P`). The Anthropic path is
unaffected. Empty/unset keeps the standard bearer header.

## Authentication Methods

### OAuth (Claude.ai Login)

Uses OAuth 2.0 Authorization Code with PKCE (S256). The token is sent as
`Authorization: Bearer <token>` and enables Pro/Max subscription billing.

**OAuth client config** (`include/agentty/auth.hpp:26-34`, `OAuthConfig`):

| Field | Value |
|---|---|
| Client ID | `9d1c250a-e61b-44d9-88ed-5944d1962f5e` |
| Authorize URL | `https://claude.ai/oauth/authorize` |
| Token URL | `https://platform.claude.com/v1/oauth/token` |
| Callback URL | `https://platform.claude.com/oauth/code/callback` |
| Scopes | `user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload` |

This is the same OAuth client as Claude Code, which enables token reuse via the
`CLAUDE_CODE_OAUTH_TOKEN` environment variable.

### API Key

Standard Anthropic API keys (`sk-ant-...`). Sent as `x-api-key` header. Uses API billing.

## Credential Resolution

On startup, `auth::resolve(cli_api_key)` (`src/auth.cpp:337-378`) walks this
priority order:

1. **`-k` / `--key`** — CLI flag, API key for a single session
2. **`ANTHROPIC_API_KEY`** env var — API key
3. **`CLAUDE_CODE_OAUTH_TOKEN`** env var — OAuth token (reuse Claude Code's auth)
4. **Credentials file** — `~/.config/agentty/credentials.json`
   (or `$XDG_CONFIG_HOME/agentty/credentials.json`)

If a stored OAuth token is expired and a refresh token is present, `resolve()`
refreshes inline before returning. If the refresh fails (or there is no refresh
token), it returns an empty `Credentials` and `main()` aborts with a "not
authenticated" message.

## Credentials File

**Path:** `~/.config/agentty/credentials.json` (mode `0600`, set via
`restrict_perms()` → `chmod` on POSIX; best-effort on Windows).

```json
{
  "method": "oauth",
  "access_token": "<token>",
  "refresh_token": "<refresh_token>",
  "expires_at": 1712345678000
}
```

- `method` — `"oauth"` or `"api_key"`
- `expires_at` — Unix milliseconds; `0` means no expiration info (API keys never expire)

`config_dir()` honours `XDG_CONFIG_HOME`, then falls back to `$HOME/.config`,
then `$USERPROFILE/.config` on Windows. The directory is created on first read
(`src/io/auth.cpp`) and `chmod 0700`'d best-effort.

### At-Rest Encryption (opt-in)

By default the credentials file is plaintext JSON protected only by `0600`
file permissions — fine on a single-user machine, but readable by any process
running as the same uid. Two independent, opt-in hardening layers are
available (`src/io/cred_crypt.cpp`, `include/agentty/auth/cred_crypt.hpp`):

**1. Passphrase-derived encryption.** When enabled, the credential blob is
sealed with AES-256-GCM under a key derived from a passphrase. The on-disk
envelope (v2) stamps the KDF and its parameters so old files keep opening.

| Env var | Effect |
|---|---|
| `AGENTTY_ENCRYPT_PASSPHRASE=1` | Prompt for a passphrase on `/dev/tty` (echo off) and seal new writes |
| `AGENTTY_PASSPHRASE=<secret>` | Supply the passphrase non-interactively (CI/scripts) |
| `AGENTTY_KDF=scrypt` | Force the portable scrypt KDF instead of Argon2id |

KDF selection is automatic: **Argon2id** (t=3, m=64 MiB, p=1) when the linked
OpenSSL exposes it (≥ 3.2, probed at runtime via `EVP_KDF_fetch("ARGON2ID")`),
otherwise **scrypt** (N=2¹⁵, r=8, p=1), the portable floor available since
OpenSSL 3.0. Unsealing dispatches on the KDF recorded in the envelope, so a
file written on an Argon2id-capable build still opens on a scrypt-only one only
if it was written with scrypt — mixing is not silent, it fails closed. Wrong
passphrase or a v2 file opened without a passphrase both refuse rather than
returning garbage.

**2. OS keystore** — see the next section. The two can be combined; when both
are on, the keystore stores the (already passphrase-sealed) blob.

## OS Keystore (opt-in)

Set `AGENTTY_USE_KEYSTORE=1` to store the credential blob in the platform
secret store instead of (well, in addition to) the file
(`src/io/keystore.cpp`, `include/agentty/auth/keystore.hpp`):

| Platform | Backend | Mechanism |
|---|---|---|
| Linux | libsecret / Secret Service | `secret-tool store/lookup/clear`, secret fed over **stdin** |
| macOS | Keychain | `security {add,find,delete}-generic-password` |
| Windows | Credential Manager | `CredWriteW` / `CredReadW` / `CredDeleteW` (DPAPI-backed) |

Integration in `src/io/auth.cpp`:

- `load_credentials` prefers the keystore, then falls back to the file.
- `save_credentials` writes **both** the keystore and the file.
- `clear_credentials` (`agentty logout`) removes the keystore item and deletes
  the file.

When no backend is available (no `secret-tool`, headless macOS, etc.) the
keystore silently reports "unsupported" and agentty falls back to the file —
enabling the flag never breaks login.

**Secret hygiene.** The secret is never placed in a command line where `ps`
or the process table could reveal it:

- Linux `secret-tool store` reads the secret from **stdin**.
- macOS `security add-generic-password -w` is invoked with **no value**, so
  `security` prompts for the password via `readpassphrase()`. The child is
  spawned into its own session (`POSIX_SPAWN_SETSID`) so it has no controlling
  tty and `readpassphrase` falls back to the stdin pipe agentty feeds — the
  secret never reaches argv.
- Windows passes the blob through the `CredWriteW` API, not a command line.

`agentty status` reports both layers (`At-rest encryption:` and `OS keystore:`
lines) so you can confirm what's active.

## TLS / Certificate Trust

`auth::apply_tls_options(curl)` is called on every libcurl handle agentty creates
(`src/io/auth.cpp`). It honours:

| Env var | Effect |
|---|---|
| `CURL_CA_BUNDLE` | `CURLOPT_CAINFO = $CURL_CA_BUNDLE` |
| `SSL_CERT_FILE` | Same, used only if `CURL_CA_BUNDLE` is unset |
| `AGENTTY_INSECURE=1` | Disables `SSL_VERIFYPEER` and `SSL_VERIFYHOST` (debug only) |
| `AGENTTY_TLS_PINS` | Opt-in public-key (SPKI) pinning — see below |

Both `CURL_CA_BUNDLE` and `SSL_CERT_FILE` are commonly set by Nix, certain
corporate VPN proxies, and tools like `mitmproxy`. `AGENTTY_INSECURE` should never
be set against a real Anthropic endpoint.

### Public-Key Pinning (`AGENTTY_TLS_PINS`)

Off by default. When set, agentty installs a custom verify callback
(`pin_verify_cb`, `src/io/tls.cpp`) on top of the normal chain verification
(`SSL_VERIFY_PEER`): after the platform trust store validates the chain, the
leaf certificate's **Subject Public Key Info** is SHA-256'd and compared
against the configured pin set. If none match, the handshake fails closed.

```bash
# One or more base64(SHA-256(SPKI)) values, comma-separated.
# Include a backup pin (next cert's key) so rotation can't brick clients.
export AGENTTY_TLS_PINS="LQfSFZEKft9yS7oIKOIO5Vu7Fj33L2H3SDN8/uADlWg=,<backup>"
```

The pin format is HPKP-style (base64 SHA-256 of the DER SPKI, matching
`openssl x509 -pubkey | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64`).
Because a bad pin makes the endpoint unreachable, this is opt-in and expects
the operator to manage rotation with a backup pin.

## OAuth PKCE Flow

The interactive login (`agentty login` → option 1) implements Authorization Code
with PKCE (`src/io/auth.cpp`, `cmd_login()`). `random_urlsafe()` draws from
OpenSSL `RAND_bytes` (CSPRNG) with an unbiased 6-bit mask:

```
┌─ agentty ────────────────────────────────────────────────────┐
│                                                           │
│  1. verifier  = random_urlsafe(128)                       │
│  2. challenge = base64url_no_pad(SHA-256(verifier))       │
│  3. state     = random_urlsafe(32)                        │
│  4. Build authorize URL with PKCE params + code=true      │
│  5. Open browser (open / xdg-open / ShellExecuteA)        │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │
                            ▼
┌─ Browser ─────────────────────────────────────────────────┐
│                                                           │
│  User logs in at claude.ai/oauth/authorize                │
│  Redirected to platform.claude.com/oauth/code/callback    │
│  Page displays "<code>#<state>" for the user to copy      │
│                                                           │
└───────────────────────────┬───────────────────────────────┘
                            │ user pastes "<code>#<state>"
                            ▼
┌─ agentty ────────────────────────────────────────────────────┐
│                                                           │
│  6. exchange_code() splits on '#'; the code half is kept and
│     the echoed state half is constant-time-compared against the
│     expected state (fails closed on mismatch, when echoed).
│     (src/io/auth.cpp)                                      │
│                                                           │
│  7. POST application/x-www-form-urlencoded to             │
│     platform.claude.com/v1/oauth/token:                   │
│       grant_type    = "authorization_code"                │
│       code          = "<pasted code>"                     │
│       client_id     = "9d1c250a-..."                      │
│       redirect_uri  = "https://platform.claude.com/..."   │
│       code_verifier = "<128-char verifier>"               │
│       state         = "<32-char random>"                  │
│                                                           │
│  8. Receive: { access_token, refresh_token, expires_in }  │
│  9. expires_at_ms = now_ms() + expires_in * 1000          │
│ 10. save_credentials() → ~/.config/agentty/credentials.json  │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

The authorize URL is built with `&code=true`, which is what Claude's OAuth
endpoint uses to render the code on a copy-paste page rather than redirecting
the browser to localhost.

## Token Refresh

OAuth tokens expire (typically after 1 hour). `auth::resolve()` checks
`is_expired()` on startup and calls `refresh_access_token()` if a refresh token
is available (`src/auth.cpp:321-331`):

```
POST https://platform.claude.com/v1/oauth/token
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token
&client_id=9d1c250a-e61b-44d9-88ed-5944d1962f5e
&refresh_token=<saved_refresh_token>
```

The response provides a new `access_token` and `expires_in`, and optionally a
new `refresh_token`. agentty persists the refreshed values to disk and continues.

There is no in-flight refresh during a streaming request — if the token expires
mid-conversation, the server returns 401 and the user must restart agentty (which
will refresh on the next startup).

## How Auth Headers Are Attached

The `anthropic::Request` struct carries the resolved auth header
(`include/agentty/anthropic.hpp:21-31`):

```cpp
struct Request {
    std::string model;
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    int max_tokens = 8192;

    // Filled by caller from auth::resolve().
    std::string auth_header;             // "Bearer <t>" or raw API key
    auth::Style auth_style = auth::Style::ApiKey;
};
```

`main.cpp:142-143` populates these from the resolved credentials:

```cpp
req.auth_header = g_creds.header_value();   // "Bearer <t>" for OAuth, raw key otherwise
req.auth_style  = g_creds.style();          // Style::Bearer or Style::ApiKey
```

`anthropic::run_stream_sync()` then chooses the header name and beta flags
(`src/anthropic.cpp:203, 240-253`):

```cpp
const bool is_oauth = (req.auth_style == auth::Style::Bearer);

std::string auth_hdr = is_oauth
    ? (std::string("Authorization: ") + req.auth_header)
    : (std::string("x-api-key: ") + req.auth_header);
headers = curl_slist_append(headers, auth_hdr.c_str());
headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
if (is_oauth) {
    headers = curl_slist_append(headers,
        "anthropic-beta: oauth-2025-04-20,prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
} else {
    headers = curl_slist_append(headers,
        "anthropic-beta: prompt-caching-2024-07-31,"
        "context-management-2025-06-27,compact-2026-01-12");
}
```

The OAuth path adds `oauth-2025-04-20` to the `anthropic-beta` list — the API
key path omits it.

### OAuth System Prompt Billing Header

When `is_oauth`, `run_stream_sync()` rewrites `body["system"]` from a plain
string into a two-element text-block array, prepending a billing-tracking block
(`src/anthropic.cpp:210-227`):

```json
[
  { "type": "text",
    "text": "x-anthropic-billing-header: cc_version=0.1.0; cc_entrypoint=cli; cch=00000;" },
  { "type": "text",
    "text": "<actual system prompt>",
    "cache_control": { "type": "ephemeral" } }
]
```

This is **not** an HTTP header — it's a content block in the request body.
Despite the `x-anthropic-billing-header:` prefix, the API recognises the block
itself and routes the request to Pro/Max subscription billing.

API-key requests skip this entirely: `body["system"] = req.system_prompt;` (a plain string).

## Error Handling

Authentication failures (HTTP 401/403) bubble up as a `StreamError` from
`run_stream_sync()` and surface in the TUI. The user must run `agentty login`
again (or fix the env var). They are **not** automatically retried — there's no
recovery agentty can do for a bad/revoked token mid-stream.

Network errors and overload (5xx) within a single request are surfaced as
`StreamError` events to the UI loop, which is responsible for any retry policy.

## Data Structures

### `Credentials` (`include/agentty/auth.hpp:13-23`)

```cpp
enum class Method { None, ApiKey, OAuth };
enum class Style  { ApiKey, Bearer };

struct Credentials {
    Method method = Method::None;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at_ms = 0;          // 0 = no expiration info (api_key)

    bool is_valid() const;              // access_token not empty
    bool is_expired() const;            // OAuth-only; checks now_ms() >= expires_at_ms
    std::string header_value() const;   // "Bearer <t>" for OAuth, raw key otherwise
    Style style() const;                // OAuth → Bearer, ApiKey → ApiKey
};
```

JSON disk format uses `"expires_at"` as the key for the `expires_at_ms` field
(historical naming — value is still milliseconds).

### `OAuthConfig` (`include/agentty/auth.hpp:26-34`)

A `struct` of `static constexpr const char*` constants for the OAuth client.
Not instantiated — accessed as `OAuthConfig::client_id`, etc.

### `TokenResponse` (`include/agentty/auth.hpp:56-62`)

Result of `exchange_code()` and `refresh_access_token()`:

```cpp
struct TokenResponse {
    bool ok = false;
    std::string error;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in_s = 0;
};
```

## Full Flow: Startup to Authenticated API Call

```
main()                                                    (src/main.cpp:1400)
  ├─ parse args                                           — -k, -m, subcommand
  ├─ subcommand?
  │   ├─ "login"   → auth::cmd_login()                   (src/auth.cpp:409)
  │   ├─ "logout"  → auth::cmd_logout()                  (src/auth.cpp:480)
  │   ├─ "status"  → auth::cmd_status()                  (src/auth.cpp:493)
  │   └─ "help"    → print_usage()
  │
  ├─ g_creds = auth::resolve(cli_key)                    (src/auth.cpp:337)
  │   ├─ -k flag?                  → ApiKey
  │   ├─ ANTHROPIC_API_KEY?        → ApiKey
  │   ├─ CLAUDE_CODE_OAUTH_TOKEN?  → OAuth (no refresh token)
  │   ├─ credentials.json?         → load_credentials()
  │   │   └─ OAuth + expired + refresh_token
  │   │       → refresh_access_token() + save_credentials()
  │   └─ none / refresh failed     → Credentials{} (invalid)
  │
  ├─ if !g_creds.is_valid() → print "not authenticated" + exit 1
  │
  ├─ persistence::load_settings(), apply --model override
  │
  └─ maya::run<AgenttyApp>()                                 — TUI event loop
        │
        └─ user submits message
              │
              └─ build anthropic::Request:
                  req.auth_header = g_creds.header_value()
                  req.auth_style  = g_creds.style()
                    │
                    └─ anthropic::run_stream_sync(req, sink)
                          ├─ is_oauth = (auth_style == Style::Bearer)
                          ├─ if is_oauth: prepend billing-header text block
                          ├─ Authorization: Bearer ...   (or)  x-api-key: ...
                          ├─ anthropic-beta: + oauth-2025-04-20 if OAuth
                          ├─ POST https://api.anthropic.com/v1/messages
                          └─ stream SSE events back through sink
```

## File Index

| File | Purpose |
|---|---|
| `include/agentty/auth.hpp` | Public types: `Method`, `Style`, `Credentials`, `OAuthConfig`, `TokenResponse`; function declarations |
| `include/agentty/auth/cred_crypt.hpp` | Passphrase at-rest encryption interface (`seal`/`unseal`, `passphrase_active`) |
| `include/agentty/auth/keystore.hpp` | OS keystore interface (`store`/`retrieve`/`remove`, `Status`) |
| `src/io/auth.cpp` | All auth logic: paths, TLS opts, load/save/clear (keystore-aware), PKCE helpers, token exchange/refresh, `resolve()`, login/logout/status subcommands |
| `src/io/tls.cpp` | libcurl TLS options + opt-in SPKI pinning (`pin_verify_cb`) |
| `src/io/cred_crypt.cpp` | Passphrase-derived AES-256-GCM sealing; scrypt / Argon2id KDF with runtime probe |
| `src/io/keystore.cpp` | Linux libsecret / macOS security / Windows CredMan backends |
| `include/agentty/anthropic.hpp` | `Request` struct (carries `auth_header` + `auth_style`), `run_stream_sync()` |
| `src/anthropic.cpp` | Attaches auth headers, rewrites system prompt with billing block for OAuth |
| `src/main.cpp` | CLI arg parsing, subcommand dispatch, calls `auth::resolve()`, wires `Credentials` into each `Request` |
