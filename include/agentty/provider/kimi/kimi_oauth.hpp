#pragma once
// agentty::provider::kimi — Kimi Code OAuth (device flow).
//
// Kimi Code (Moonshot AI) authenticates terminal clients with the OAuth 2.0
// Device Authorization Grant (RFC 8628) against `https://auth.kimi.com`, then
// serves an OpenAI-compatible chat API at `https://api.kimi.com/coding/v1`.
//
// Unlike GitHub Copilot, Kimi's device-flow access_token is used DIRECTLY as
// the API bearer — there is no separate proxy-token exchange step. When the
// access_token expires it is refreshed via the standard `refresh_token` grant.
// So this module is the simpler sibling of copilot_oauth: request a device
// code, poll for the token bundle, persist it, and refresh transparently.
//
// Three OAuth endpoints, all POST form-encoded to the OAuth host:
//   POST /api/oauth/device_authorization        → device + user code
//   POST /api/oauth/token (grant=device_code)   → token bundle (polling)
//   POST /api/oauth/token (grant=refresh_token) → refreshed token bundle
//
// The persisted bundle lives in `config_dir()/kimi_credentials.json`, sealed
// with the same cred_crypt seam as every other provider credential.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentty/auth/auth.hpp"   // OAuthError

namespace agentty::provider::kimi {

// The persisted Kimi token bundle. `access_token` is the API bearer;
// `refresh_token` mints a fresh one when it expires.
struct KimiToken {
    std::string access_token;        // send as `Authorization: Bearer <token>`
    std::string refresh_token;       // used with grant_type=refresh_token
    std::int64_t expires_at_ms = 0;  // skew-safe LOCAL expiry (now + expires_in)
    std::string token_type;          // "Bearer"
    std::string scope;               // granted scopes (informational)

    [[nodiscard]] bool expired(std::int64_t skew_ms = 0) const noexcept {
        return expires_at_ms != 0 && now_ms() >= expires_at_ms - skew_ms;
    }
    [[nodiscard]] bool valid() const noexcept { return !access_token.empty(); }

    static std::int64_t now_ms() noexcept;
};

// The UI seam: agentty shows this to the user ("go to <url> and enter <code>").
struct DeviceCode {
    std::string verification_uri;            // https://auth.kimi.com/device (fallback)
    std::string verification_uri_complete;   // pre-filled URL with the code embedded
    std::string user_code;                   // e.g. "WDJB-MJHT"
    int         expires_in = 900;
};
using DeviceCodeSink = std::function<void(const DeviceCode&)>;
using CancelProbe    = std::function<bool()>;

// ── Device-flow login ─────────────────────────────────────────────────────
// Requests a device code (delivered to `on_device_code` so the modal can show
// it), then block-polls the token endpoint until the user approves, the code
// expires, or `cancelled()` trips. On success the token bundle is persisted
// and returned. `timeout_s` bounds the whole poll loop.
[[nodiscard]] std::expected<KimiToken, auth::OAuthError>
login(int timeout_s, DeviceCodeSink on_device_code, CancelProbe cancelled);

// ── Credential store ───────────────────────────────────────────────────────
[[nodiscard]] std::filesystem::path credentials_path();
[[nodiscard]] std::optional<KimiToken> load_token();
bool save_token(const KimiToken& tok);
bool clear_credentials();

// Cheap "is a Kimi credential present?" for the picker view (stat-cached).
[[nodiscard]] bool signed_in();

// Force the next fresh_token() to refresh even if the cached token looks valid
// (called after the API returns 401 mid-turn).
void invalidate_cached_token();

// ── Per-turn token ─────────────────────────────────────────────────────────
// Returns a valid access token for the API, refreshing via refresh_token when
// the persisted one is expired (or a forced refresh was requested). nullopt
// when not signed in or the refresh failed.
[[nodiscard]] std::optional<KimiToken> fresh_token();

// ── Testing seam ───────────────────────────────────────────────────────────
// Parse a token-endpoint JSON body into a KimiToken (expiry computed from
// `expires_in` relative to `now_ms`). nullopt when the required fields are
// missing. Exposed so the wire parsing is unit-testable without a network.
[[nodiscard]] std::optional<KimiToken>
parse_token_response(std::string_view json_body, std::int64_t now_ms);

// ── Device identity headers ─────────────────────────────────────────────────
// The X-Msh-* header block Kimi's servers expect from the kimi_code_cli client
// on OAuth AND API (models / chat) requests: platform, version, device name /
// model / os, and a stable per-machine device id. Kept in one place so the
// login flow and the inference transport send an identical set. Returns
// lowercase (name, value) pairs.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> device_headers();

} // namespace agentty::provider::kimi
