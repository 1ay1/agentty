#pragma once
// agentty::mcp::oauth — interactive OAuth 2.1 login for MCP servers (spec
// 2026-07-28 authorization). Drives the full browser flow and persists an
// issuer-bound token per server, refreshed on expiry.
//
//   The PROTOCOL LOGIC lives in mcp-cpp's <mcp/auth.hpp> (PKCE, RFC 9207 iss
//   validation, DCR/CIMD, token parsing) — pure + unit-tested. THIS module is
//   the agentty-side I/O + lifecycle: agentty's HTTP client for every GET/POST,
//   a cross-platform loopback callback server, the default browser open, and
//   an at-rest-encrypted token store (auth::crypt, chmod 600) keyed by server.
//
//   Flow (agentty mcp-login <server>):
//     1. resolve <server> → endpoint URL from .agentty/mcp.json
//     2. probe the endpoint → 401 → RFC 9728 resource-metadata URL
//        (or accept an explicit --metadata URL)
//     3. GET resource metadata → authorization server → GET its metadata
//     4. Dynamic Client Registration (application_type=native) [or CIMD]
//     5. open browser at the PKCE authorize URL; catch the loopback redirect
//     6. validate state + RFC 9207 iss, exchange code → issuer-bound token
//     7. seal + persist under ~/.agentty/mcp_tokens/<server>.json
//
//   At call time the HTTP transport asks bearer_for(server) for a fresh
//   Authorization header; an expired token is transparently refreshed.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace agentty::mcp::oauth {

// One persisted, issuer-bound token record for a server (mirrors
// mcp::auth::AccessToken + refresh bookkeeping).
struct StoredToken {
    std::string  access_token;
    std::string  token_type = "Bearer";
    std::string  refresh_token;
    std::string  issuer;            // AS that minted it (replay guard)
    std::string  resource;          // aud
    std::string  client_id;         // for refresh
    std::int64_t expires_at_ms = 0; // 0 ⇒ no expiry known

    [[nodiscard]] std::string authorization_header() const {
        return token_type + " " + access_token;
    }
    [[nodiscard]] bool expired(std::int64_t skew_ms = 60'000) const noexcept {
        return expires_at_ms != 0 &&
               now_ms() >= expires_at_ms - skew_ms;
    }
    static std::int64_t now_ms() noexcept;
};

// Result of a login attempt.
struct LoginResult {
    bool        ok = false;
    std::string message;   // human-readable success/failure detail
};

// Run the interactive login for `server_name` (must exist in mcp.json as an
// http/url server). If `metadata_url` is non-empty it's used directly instead
// of probing for a 401 challenge. Blocks on the browser round-trip up to
// `timeout_s`. Persists the token on success. Never throws.
[[nodiscard]] LoginResult login(const std::string& server_name,
                                const std::string& endpoint_url,
                                const std::string& metadata_url,
                                int timeout_s = 300);

// Return a fresh "Bearer <token>" header for `server_name`, refreshing a token
// that's within the expiry skew. std::nullopt if no token is stored (caller
// proceeds unauthenticated) or a refresh failed with no usable stale token.
[[nodiscard]] std::optional<std::string> bearer_for(const std::string& server_name);

// Remove a stored token. Returns true if a file was removed or none existed.
bool logout(const std::string& server_name);

// True iff a token is currently stored for the server.
[[nodiscard]] bool has_token(const std::string& server_name);

// CLI entry points (agentty mcp-login <server> / mcp-logout <server>). They
// resolve the server's endpoint URL from .agentty/mcp.json themselves and
// print human-readable progress. Return a process exit code (0 = success).
int cmd_mcp_login(const std::string& server_name, const std::string& metadata_url = {});
int cmd_mcp_logout(const std::string& server_name);
int cmd_mcp_status();   // list servers + which have a stored token

} // namespace agentty::mcp::oauth
