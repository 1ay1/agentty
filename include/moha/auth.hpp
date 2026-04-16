#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace moha::auth {

enum class Method { None, ApiKey, OAuth };
enum class Style  { ApiKey, Bearer };

struct Credentials {
    Method method = Method::None;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at_ms = 0; // 0 = no expiration info (api_key)

    bool is_valid() const { return !access_token.empty(); }
    bool is_expired() const;                // only meaningful for OAuth
    std::string header_value() const;       // "Bearer <t>" or raw key
    Style style() const { return method == Method::OAuth ? Style::Bearer : Style::ApiKey; }
};

// OAuth client configuration — matches Claude Code.
struct OAuthConfig {
    static constexpr const char* client_id     = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
    static constexpr const char* authorize_url = "https://claude.ai/oauth/authorize";
    static constexpr const char* token_url     = "https://platform.claude.com/v1/oauth/token";
    static constexpr const char* redirect_uri  = "https://platform.claude.com/oauth/code/callback";
    static constexpr const char* scopes =
        "user:profile user:inference user:sessions:claude_code "
        "user:mcp_servers user:file_upload";
};

// Paths
std::filesystem::path config_dir();              // ~/.config/moha or %USERPROFILE%/.config/moha
std::filesystem::path credentials_path();

// TLS: apply CAINFO from env (CURL_CA_BUNDLE / SSL_CERT_FILE) and insecure-mode
// (MOHA_INSECURE=1) to a libcurl handle. Call this on every handle moha creates.
void apply_tls_options(void* curl_easy_handle);

// Disk I/O
std::optional<Credentials> load_credentials();
bool save_credentials(const Credentials& c);     // writes with 0600 perms where supported
bool clear_credentials();

// PKCE helpers (exposed for tests)
std::string random_urlsafe(size_t n);
std::string base64url_no_pad(const unsigned char* data, size_t len);
std::string sha256_hex(const std::string& s);    // for debug
std::string code_challenge_s256(const std::string& verifier);

// Token operations
struct TokenResponse {
    bool ok = false;
    std::string error;
    std::string access_token;
    std::string refresh_token;
    int64_t expires_in_s = 0;
};
TokenResponse exchange_code(const std::string& code,
                            const std::string& verifier,
                            const std::string& state);
TokenResponse refresh_access_token(const std::string& refresh_token);

// Resolve credentials following the documented priority order.
// `cli_api_key` (from `-k`) takes top priority if non-empty. Auto-refresh
// expired OAuth tokens when refresh_token is available.
Credentials resolve(const std::string& cli_api_key);

// Interactive CLI flows (blocking, stdout/stdin — NOT in TUI).
int cmd_login();   // exit code
int cmd_logout();
int cmd_status();

} // namespace moha::auth
