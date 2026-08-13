# GitHub Copilot — native provider integration (research + design)

Status: SHIPPED. The design below was implemented end-to-end — device-flow login
(`agentty login` → 4), skew-safe mid-session token refresh, multi-tier endpoint
routing from `endpoints.api`, and the `copilot` provider row + picker/model-list
integration. Pure token logic is covered by `copilot_token_test`.

This document remains the reference for the real, native, complete way to wire
Copilot into agentty, cross-referenced against VS Code's own
`vscode-copilot-chat` source, the `ai-provider-kit` Go provider, and the failure
reports of ~a dozen third-party integrations (opencode, openclaw, hermes-agent,
cherry-studio, avante.nvim, codecompanion, LiteLLM).

---

## 1. The shape of it

Copilot's chat API is **OpenAI Chat Completions compatible** (`POST
/chat/completions`, SSE stream, OpenAI tool-call schema). agentty already has
that dialect: `Wire::OpenAIChat` + the `openai/transport.cpp` transport. So the
*inference* path is nearly free.

The entire integration is really an **AUTH problem**. Copilot uses a two-stage
credential:

```
GitHub device-flow OAuth  ──►  a long-lived GitHub user token (ghu_…)
        (once, persisted)               │
                                        ▼  GET /copilot_internal/v2/token
                              a SHORT-LIVED Copilot proxy token (~30 min TTL)
                                        │   (auto-refreshed from refresh_in)
                                        ▼  Authorization: Bearer <proxy>
                              POST https://api.githubcopilot.com/chat/completions
```

This maps EXACTLY onto agentty's existing `oauth_native` + `LongLived` machinery
(the ChatGPT/Codex path). Copilot is "ChatGPT-shaped auth wrapping an
OpenAI-Chat-shaped wire."

---

## 2. Constants (verified against VS Code + multiple impls)

```
# OAuth — device flow (GitHub)
CLIENT_ID          = "Iv1.b507a08c87ecfe98"   # ⚠ VS Code's GitHub *App* client id.
                                              #   MUST be this exact id — see §5.
DEVICE_CODE_URL    = https://github.com/login/device/code
ACCESS_TOKEN_URL   = https://github.com/login/oauth/access_token
VERIFICATION_URI   = https://github.com/login/device
SCOPE              = "read:user"              # copilot access is entitlement-based,
                                              #   not scope-based; read:user suffices.
GRANT_TYPE (poll)  = urn:ietf:params:oauth:grant-type:device_code

# Token exchange (GitHub API)
COPILOT_TOKEN_URL  = https://api.github.com/copilot_internal/v2/token   # GET
                     # header: Authorization: token <ghu_…>   (NOTE: "token", not "Bearer")

# Inference (Copilot proxy) — but PREFER the endpoints.api from the token response (§4)
COPILOT_BASE       = https://api.githubcopilot.com
COPILOT_BUSINESS   = https://api.business.githubcopilot.com
COPILOT_ENTERPRISE = https://api.enterprise.githubcopilot.com
CHAT_PATH          = /chat/completions
MODELS_PATH        = /models

# Required request headers on EVERY inference call (400/403 without them)
Authorization         : Bearer <copilot-proxy-token>
Copilot-Integration-Id: vscode-chat
Editor-Version        : vscode/1.104.3          # any plausible vscode/<ver>
Editor-Plugin-Version : copilot-chat/0.26.7
User-Agent            : GitHubCopilotChat/0.26.7
Openai-Intent         : conversation-panel      # some models 400 without it
X-Request-Id          : <uuid v4 per request>   # recommended, aids support
# For vision/streaming a few impls also send:
Copilot-Vision-Request: true                    # only when sending image parts
```

The header block is the #1 cause of `HTTP 400 missing Editor-Version` reports.

---

## 3. Device-flow login (one time, persisted)

Identical structure to `chatgpt/codex_oauth.cpp`'s `request_device_authorization`
+ `poll_device_authorization`, just different URLs and a `token` grant.

1. `POST github.com/login/device/code`
   body `client_id=<CLIENT_ID>&scope=read:user`, `Accept: application/json`
   → `{ device_code, user_code, verification_uri, expires_in, interval }`
2. Show the user: *"Go to https://github.com/login/device and enter CODE `XXXX-XXXX`"*
   (agentty can also `open_browser(verification_uri)` like the Codex flow does).
3. Poll `POST github.com/login/oauth/access_token`
   body `client_id=<CLIENT_ID>&device_code=<dc>&grant_type=urn:ietf:params:oauth:grant-type:device_code`
   every `interval` seconds:
   - `{ "error": "authorization_pending" }` → keep polling
   - `{ "error": "slow_down" }` → increase interval by 5s
   - `{ "error": "expired_token" }` → restart from step 1
   - `{ "access_token": "ghu_…", "token_type": "bearer", "scope": "…" }` → DONE
4. Persist the `ghu_…` token. This is the durable credential (no refresh token in
   device flow — the ghu_ token is itself long-lived; if it's ever revoked the
   user re-runs login).

---

## 4. Token exchange + refresh (the short-lived proxy token)

`GET https://api.github.com/copilot_internal/v2/token`
header `Authorization: token <ghu_…>`, `Accept: application/json`,
plus the editor headers (User-Agent etc).

Success response (`TokenEnvelope`, per VS Code `copilotToken.ts`):
```jsonc
{
  "token": "tid=…;exp=…;…:<hmac>",   // the proxy token — send as Bearer
  "expires_at": 1700000000,          // unix seconds, absolute
  "refresh_in": 1500,                // seconds until you SHOULD refresh (~25 min)
  "sku": "…",                        // "free_limited_copilot" ⇒ free tier
  "chat_enabled": true,
  "endpoints": {                     // ⚠ AUTHORITATIVE base URL — use this
     "api": "https://api.githubcopilot.com",
     "telemetry": "…", "proxy": "…"
  },
  "limited_user_quotas": { "chat": 0 }  // present for free users
}
```

Refresh rules (VS Code `copilotTokenManager.ts`):
- Recompute a LOCAL expiry immune to clock skew:
  `local_expires_at = now() + refresh_in + 60`.
- Treat the token as stale when `now() >= local_expires_at - 300` (5-min buffer),
  and re-exchange BEFORE the next request. Dedupe concurrent refreshes (a
  single-flight guard — agentty already has `CrossProcessFileLock` and can add an
  in-process mutex; the subagent long-session 401 bugs in other tools all come
  from *not* refreshing mid-session, which agentty must get right because a
  Complex smart-mode turn can run >30 min).
- Route inference to `body.endpoints.api` (falls back to `api.githubcopilot.com`).
  This is how Business/Enterprise users reach the right host WITHOUT hardcoding —
  do NOT guess business/enterprise from account type (a documented 404 source).

401 on an inference call ⇒ force-refresh the proxy token once and retry; if the
refresh itself 401s, the ghu_ token is dead ⇒ surface "re-run copilot login".

---

## 5. The critical gotchas (from other tools' bug trackers)

1. **CLIENT_ID must be `Iv1.b507a08c87ecfe98`** (VS Code's GitHub *App*).
   - OAuth *Apps* mint `gho_` tokens; the `/copilot_internal/v2/token` exchange
     only accepts GitHub *App* user tokens (`ghu_`). Wrong client id ⇒ 403/404 on
     exchange. (opencode #19338, hermes #16551, openfang #1014.)
   - The server also keeps a **per-client-id model allowlist**. Using a non-VS-Code
     client id silently yields a *smaller/different* model set. Every working
     third-party tool uses this exact id. (opencode #20759.)
2. **Individual vs Business/Enterprise 404**: never hardcode the business/
   enterprise host or assume the exchange path — read `endpoints.api` from the
   token response. Some Individual Pro accounts 404 on assumptions. (hermes #27836.)
3. **Editor-Version header is mandatory** on inference — 400 without it.
   (openclaw #58056.)
4. **~30-min TTL, refresh mid-session** — long agent runs die at 401 otherwise;
   agentty's smart-mode Complex turns + subagents make this non-optional.
   (openclaw #8805, #31132.)
5. **Free tier**: `sku == "free_limited_copilot"` and `limited_user_quotas.chat`
   caps usage; surface a clean "Copilot chat quota exhausted" instead of a raw
   403.

---

## 6. How it maps onto agentty (implementation plan)

### 6a. Transport — tiny addition to the OpenAI transport
- Add `std::vector<std::pair<std::string,std::string>> extra_headers` to
  `provider::openai::Endpoint` (transport.hpp).
- In `build_request_headers`, after the auth header, append `endpoint.extra_headers`.
- These carry `Copilot-Integration-Id`, `Editor-Version`, `Editor-Plugin-Version`,
  `Openai-Intent`, `User-Agent`. (Everything else on the wire is already OpenAI-Chat.)
- `Endpoint::from_spec("copilot")` → host from token `endpoints.api`, path
  `/chat/completions`, models_path `/models`, the header block preset.

### 6b. Auth module — new `src/provider/copilot/copilot_oauth.cpp`
Mirror `chatgpt/codex_oauth.cpp` almost verbatim:
- `login()` — device flow (§3), persists the ghu_ token via the existing
  credential store (`auth::` cred_crypt, same as Codex creds).
- `copilot_token()` — returns a valid proxy token, exchanging/refreshing per §4,
  single-flight guarded. Stores `{proxy_token, local_expires_at, endpoint_api}`.
- `logout()` — clears both stored tokens.
- Reuse `open_browser`, the poll loop, `now_ms`, `post()` helpers — most of the
  file is shared shape.

### 6c. Provider wrapper — `src/provider/copilot/provider.cpp`
Thin, like `chatgpt/provider.cpp`:
- `stream(Request, EventSink)`: ensure a fresh proxy token (refresh if stale),
  set `req.auth` = proxy token, `req.endpoint` = the copilot endpoint (host from
  `endpoint_api`), then DELEGATE to `openai::stream` (it already speaks this wire).
  On a 401 from the transport, force-refresh once and retry.
- `list_models()`: `GET {endpoint_api}/models` with the same headers → the
  live per-account model catalog (gpt-4o, o1, claude-sonnet-4, gemini, etc. — the
  set depends on the account's entitlements, which is why it must be *listed*, not
  hardcoded).

### 6d. Registry — one row in `kProviders`
```
{ id: "copilot", label: "GitHub Copilot",
  wire: Wire::OpenAIChat, lifetime: Lifetime::LongLived,
  auth_style: AuthStyle::None, oauth_native: true,
  kind: Kind::Copilot /* new enum arm */ }
```

### 6e. main.cpp wiring
- Add a `Kind::Copilot` arm alongside the ChatGPT one: construct
  `copilot::CopilotProvider`, register it in the `LongLived` router under a new
  `LongLived::Copilot`.
- `cmd_login`/`cmd_logout`/`cmd_status` gain a copilot branch (device-flow UI,
  same as Codex).
- The provider picker row + model badge come for free from the registry.

### 6f. Model catalog / capabilities
- Copilot models are third-party (OpenAI o-series, Anthropic, Gemini) fronted by
  Copilot. `ModelCapabilities::from_id` should recognise the ids Copilot returns
  so effort/tool support resolve correctly; unknown ids degrade to "no effort
  control, tools on" conservatively.
- Copilot's OpenAI-Chat surface supports `tools`/`tool_choice` and streaming;
  effort/reasoning control is NOT the OpenAI `reasoning_effort` field — omit it
  (Copilot ignores/400s unknown fields on some models). So route Copilot models
  as effort-none in the catalog unless a model is known to accept it.

### 6g. Tests
- Unit: the token-envelope parser (`expires_at`/`refresh_in`/`endpoints.api`/`sku`
  extraction), the skew-safe local-expiry math, the stale/refresh decision, the
  header block assembly. All pure, no network — feed canned JSON like the
  existing `codex_responses_test`.
- The device-flow poll state machine (pending/slow_down/expired/success) with a
  scripted `post()` seam, mirroring how `codex_login_flow_test` scripts the
  Codex flow.

---

## 7. Effort estimate

- 6a (header hook): ~30 min, and it's shared with any future header-needing provider.
- 6b (oauth module): the bulk — but ~70% is a structural copy of codex_oauth.cpp.
  ~half a day.
- 6c–6e (wrapper + registry + main wiring): ~2–3 hours.
- 6f/6g (capabilities + tests): ~2–3 hours.

Total ≈ 1 focused day for a complete, native, self-refreshing integration with
device-flow login, correct multi-tier endpoint routing, and mid-session refresh.

The result: `agentty login` → "Sign in with GitHub Copilot" → device code →
Copilot appears in the provider picker with its live model list, and every
smart-mode/subagent turn refreshes the proxy token transparently.
```
