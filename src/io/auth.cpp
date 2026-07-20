#include "agentty/auth/auth.hpp"

// OAuth config lives with the provider it belongs to; `using` lets the
// existing OAuthConfig:: references below stay short.
#include "agentty/provider/anthropic/oauth.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keystore.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/env.hpp"

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
// <spawn.h> is gated behind __ANDROID_API__ >= 28 on Bionic and absent from
// the sysroot below that (Termux on some devices). Prefer posix_spawn where
// available; otherwise the fork/exec fallback in open_browser needs no header.
#  if __has_include(<spawn.h>)
#    include <spawn.h>
#    define AGENTTY_HAVE_POSIX_SPAWN 1
#  else
#    define AGENTTY_HAVE_POSIX_SPAWN 0
#  endif
extern char** environ;
#endif

namespace agentty::auth {

using OAuthConfig = agentty::provider::anthropic::OAuthConfig;

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Credentials accessors — derived views over the variant.
// std::visit + a generic lambda gives exhaustive matching: adding a new
// alternative will fail to compile here until handled.
// ---------------------------------------------------------------------------

static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool is_valid(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> bool {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return false;
        else if constexpr (std::same_as<T, cred::ApiKey>) return !v.key.empty();
        else /* cred::OAuth */                            return !v.access_token.empty();
    }, c);
}

bool is_expired(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, cred::OAuth>) {
            if (v.expires_at_ms == 0) return false;
            return now_ms() >= v.expires_at_ms;
        } else {
            return false;   // None / ApiKey never expire
        }
    }, c);
}

std::string header_value(const Credentials& c) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return {};
        else if constexpr (std::same_as<T, cred::ApiKey>) return v.key;
        else /* cred::OAuth */ return std::string{"Bearer "} + v.access_token;
    }, c);
}

Style style(const Credentials& c) noexcept {
    return std::holds_alternative<cred::OAuth>(c) ? Style::Bearer : Style::ApiKey;
}

AuthHeader make_auth_header(const Credentials& c) {
    return std::visit([](const auto& v) -> AuthHeader {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, cred::None>) {
            // Empty ApiKeyHeader is the canonical "unauthenticated" arm —
            // is_empty(...) catches it before the transport dials.
            return AuthHeader{ApiKeyHeader{}};
        } else if constexpr (std::same_as<T, cred::ApiKey>) {
            return AuthHeader{ApiKeyHeader{v.key}};
        } else /* cred::OAuth */ {
            return AuthHeader{BearerHeader{v.access_token}};
        }
    }, c);
}

bool is_empty(const AuthHeader& h) noexcept {
    return std::visit([](const auto& a) noexcept {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::same_as<T, ApiKeyHeader>) return a.value.empty();
        else /* BearerHeader */                      return a.token.empty();
    }, h);
}

std::string_view persist_tag(const Credentials& c) noexcept {
    return std::visit([](const auto& v) noexcept -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, cred::None>)   return "none";
        else if constexpr (std::same_as<T, cred::ApiKey>) return "api_key";
        else /* cred::OAuth */                            return "oauth";
    }, c);
}

std::string OAuthError::render() const {
    std::string_view kind_str;
    switch (kind) {
        case OAuthErrorKind::Network:      kind_str = "network";      break;
        case OAuthErrorKind::BadResponse:  kind_str = "bad response"; break;
        case OAuthErrorKind::ApiError:     kind_str = "api error";    break;
        case OAuthErrorKind::MissingToken: kind_str = "missing token";break;
    }
    return "[" + std::string{kind_str} + "] " + detail;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) home = std::getenv("USERPROFILE");
        base = (home && *home) ? fs::path(home) / ".config" : fs::current_path() / ".config";
    }
    fs::path p = base / "agentty";
    std::error_code ec;
    fs::create_directories(p, ec);
    // Tighten to owner-only (SECURITY_AUDIT): the directory holds
    // credentials.json; a world-listable/traversable parent is an
    // unnecessary information leak even though the file itself is 0600.
    // Best-effort — chmod failures (e.g. Windows, restricted FS) are
    // non-fatal; the file's own 0600 remains the real barrier.
#ifndef _WIN32
    ::chmod(p.c_str(), S_IRWXU);
#endif
    return p;
}

fs::path credentials_path() { return config_dir() / "credentials.json"; }

// ---------------------------------------------------------------------------
// Pre-warm: open TCP+TLS+h2 to api.anthropic.com while the user is still
// typing, so the first real request skips the handshake. The http::Client's
// pool keeps the connection until used or until the 90 s idle TTL elapses.
// ---------------------------------------------------------------------------
void prewarm_anthropic() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    const auto& ov = http::agentty_api_host_override();
    http::default_client().prewarm("api.anthropic.com", 443,
                                   ov.active() ? ov.host : std::string{},
                                   ov.active() ? ov.port : uint16_t{0});
}

// ---------------------------------------------------------------------------
// Load/save/clear credentials
// ---------------------------------------------------------------------------

static void restrict_perms(const fs::path& p) {
#ifdef _WIN32
    (void)p; // best-effort — Windows ACLs are out of scope here
#else
    ::chmod(p.c_str(), S_IRUSR | S_IWUSR);
#endif
}

// POSIX: create the file with mode 0600 from the start so there is no window
// where it exists world-readable between open() and chmod(), and write it
// ATOMICALLY (SECURITY_AUDIT) via a sibling temp file + fsync + rename so a
// crash mid-write can never leave a truncated/corrupt credentials.json.
// Windows: fall back to std::ofstream (ACLs are out of scope here).
static bool write_private(const fs::path& p, const std::string& content) {
#ifdef _WIN32
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), (std::streamsize)content.size());
    return static_cast<bool>(ofs);
#else
    // Temp path in the SAME directory as the target so rename(2) is atomic
    // (rename across filesystems is not). Include the pid to avoid clobbering
    // a concurrent writer's temp.
    fs::path tmp = p;
    tmp += ".tmp." + std::to_string(::getpid());

    int fd = ::open(tmp.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR);
    if (fd < 0) return false;

    const char* buf = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        buf += n;
        remaining -= (size_t)n;
    }
    // Flush the data to disk before the rename so the rename can't land
    // pointing at a file whose contents haven't been persisted yet.
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    // Atomic replace: readers see either the old file or the fully-written
    // new one, never a partial write.
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
#endif
}

std::optional<Credentials> load_credentials() {
    std::string raw;
    // Prefer the OS keystore when enabled — it holds the sealed envelope.
    // Fall back to the on-disk file when the keystore is off, empty, or the
    // item is missing (e.g. first run after opting in).
    if (keystore::available()) {
        std::string ks;
        if (keystore::retrieve("credentials", ks) == keystore::Status::Ok
            && !ks.empty()) {
            raw = std::move(ks);
        }
    }
    if (raw.empty()) {
        std::ifstream ifs(credentials_path(), std::ios::binary);
        if (!ifs) return std::nullopt;
        raw.assign((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    }
    if (raw.empty()) return std::nullopt;

    // Encrypted-at-rest (SECURITY_AUDIT #1). New files are sealed
    // envelopes; a pre-encryption file is plaintext JSON. Detect which,
    // decrypt the sealed case, and fall through to the SAME parser for
    // both — a legacy plaintext file is transparently accepted and gets
    // re-encrypted the next time save_credentials runs (token refresh /
    // re-login), so no explicit migration step is needed.
    std::string body;
    if (crypt::looks_sealed(raw)) {
        auto pt = crypt::unseal(raw);
        if (!pt) return std::nullopt;   // tampered / wrong machine / corrupt
        body = std::move(*pt);
    } else {
        body = std::move(raw);          // legacy plaintext
    }

    try {
        json j = json::parse(body);
        auto m = j.value("method", "");
        if (m == "oauth") {
            cred::OAuth o;
            o.access_token  = j.value("access_token", "");
            o.refresh_token = j.value("refresh_token", "");
            o.expires_at_ms = j.value("expires_at", std::int64_t{0});
            if (o.access_token.empty()) return std::nullopt;
            return Credentials{std::move(o)};
        }
        if (m == "api_key") {
            cred::ApiKey k;
            k.key = j.value("access_token", "");    // legacy field name
            if (k.key.empty()) return std::nullopt;
            return Credentials{std::move(k)};
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool save_credentials(const Credentials& c) {
    json j;
    j["method"] = std::string{persist_tag(c)};
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::same_as<T, cred::ApiKey>) {
            j["access_token"]  = v.key;
            j["refresh_token"] = "";
            j["expires_at"]    = std::int64_t{0};
        } else if constexpr (std::same_as<T, cred::OAuth>) {
            j["access_token"]  = v.access_token;
            j["refresh_token"] = v.refresh_token;
            j["expires_at"]    = v.expires_at_ms;
        } else {
            j["access_token"]  = "";
            j["refresh_token"] = "";
            j["expires_at"]    = std::int64_t{0};
        }
    }, c);
    fs::path p = credentials_path();
    // Encrypt at rest (SECURITY_AUDIT #1). If sealing fails (hard OpenSSL
    // error), refuse to persist rather than silently writing plaintext
    // tokens — the in-memory credential still works for this session, and
    // the caller surfaces the save failure.
    std::string payload = j.dump(2);
    auto sealed = crypt::seal(payload);
    if (!sealed) return false;
    // When the OS keystore is enabled, it becomes the primary store for the
    // sealed envelope. We still write the encrypted file too, so a later run
    // with the keystore disabled (or a keychain that won't unlock) can still
    // load. The file is already machine-bound-encrypted, so this is not a
    // downgrade — it's the same protection that shipped before the keystore.
    if (keystore::available())
        keystore::store("credentials", *sealed);
    if (!write_private(p, *sealed)) return false;
    restrict_perms(p);
    return true;
}

bool clear_credentials() {
    if (keystore::available())
        keystore::remove("credentials");
    std::error_code ec;
    fs::remove(credentials_path(), ec);
    return !ec;
}

// ---------------------------------------------------------------------------
// PKCE helpers
// ---------------------------------------------------------------------------

std::string base64url_no_pad(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        out.push_back(tbl[(v >> 6) & 0x3f]);
        out.push_back(tbl[v & 0x3f]);
        i += 3;
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i+1] << 8;
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        if (i + 1 < len) out.push_back(tbl[(v >> 6) & 0x3f]);
    }
    return out;
}

// Cryptographically-secure URL-safe random string (SECURITY_AUDIT: PKCE).
// The OAuth code_verifier and anti-CSRF state MUST be unpredictable — a
// non-CSPRNG (mt19937) is recoverable from a handful of outputs, which would
// let an attacker forge a verifier or predict state. RAND_bytes is the OpenSSL
// CSPRNG (already linked; used in cred_crypt.cpp). We draw one secure byte per
// output char and fold it into the 64-char alphabet: 64 divides 256 evenly, so
// the modulo is unbiased (each char equally likely). RAND_bytes failure is
// treated as fatal — better to abort login than to emit low-entropy secrets.
std::string random_urlsafe(size_t n) {
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    // charset has 64 usable chars (sizeof-1 for the NUL); 256 % 64 == 0 so
    // masking the low 6 bits of a uniform byte is itself uniform — no bias,
    // no rejection loop needed.
    static_assert(sizeof(charset) - 1 == 64, "alphabet must be 64 for unbiased mask");
    std::vector<unsigned char> buf(n);
    if (n > 0 && RAND_bytes(buf.data(), static_cast<int>(n)) != 1) {
        // CSPRNG unavailable — do NOT fall back to a weak generator.
        throw std::runtime_error("RAND_bytes failed: no secure entropy source");
    }
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(charset[buf[i] & 0x3f]);
    return out;
}

std::string sha256_hex(const std::string& s) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), md);
    std::ostringstream oss;
    oss << std::hex;
    for (unsigned char c : md) oss << (c < 16 ? "0" : "") << (int)c;
    return oss.str();
}

std::string code_challenge_s256(const std::string& verifier) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
             verifier.size(), md);
    return base64url_no_pad(md, SHA256_DIGEST_LENGTH);
}

// ---------------------------------------------------------------------------
// HTTP helpers (form-encoded POST against the OAuth endpoint)
// ---------------------------------------------------------------------------

namespace {

// RFC 3986 unreserved set passes through; everything else gets %HH-encoded.
// Mirrors curl_easy_escape's behaviour for the same input set.
std::string url_escape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
         || (c >= '0' && c <= '9')
         || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string form_urlencode(const std::vector<std::pair<std::string,std::string>>& kv) {
    std::string out;
    for (size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += url_escape(kv[i].first);
        out += '=';
        out += url_escape(kv[i].second);
    }
    return out;
}

// Endpoint parser for the small set of OAuth URLs we hit. We don't pull in a
// general URL parser: every URL we use is https://, no userinfo, no fragment.
struct ParsedUrl {
    std::string host;
    uint16_t    port = 443;
    std::string path;   // includes leading '/' and any query string
};

std::expected<ParsedUrl, std::string> parse_https_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme)
        return std::unexpected(std::string{"missing https:// scheme"});
    url.remove_prefix(scheme.size());
    auto slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    ParsedUrl p;
    p.path = std::string{slash == std::string_view::npos ? "/" : url.substr(slash)};
    auto colon = authority.find(':');
    if (colon == std::string_view::npos) {
        p.host = std::string{authority};
    } else {
        p.host = std::string{authority.substr(0, colon)};
        try {
            int port_int = std::stoi(std::string{authority.substr(colon + 1)});
            if (port_int <= 0 || port_int > 65535)
                return std::unexpected(std::string{"port out of range"});
            p.port = static_cast<uint16_t>(port_int);
        } catch (...) { return std::unexpected(std::string{"bad port"}); }
    }
    if (p.host.empty()) return std::unexpected(std::string{"empty host"});
    return p;
}

// Internal HTTP form-POST result. Distinct from the public TokenResult so
// the typed mapping (HTTP error → OAuthError) happens in exactly one place
// (parse_token_json below).
struct FormPostResult {
    int         status = 0;
    std::string body;
    std::string transport_error;   // non-empty iff request never reached server
};

FormPostResult http_post_form(const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& fields) {
    FormPostResult r;
    auto parsed = parse_https_url(url);
    if (!parsed) {
        r.transport_error = "bad url: " + url + " (" + parsed.error() + ")";
        return r;
    }

    http::Request hreq;
    hreq.method = http::HttpMethod::Post;
    hreq.host   = parsed->host;
    hreq.port   = parsed->port;
    // Air-gapped users tunnel the OAuth host (token exchange + refresh)
    // through SSH the same way they tunnel the API host — see AGENTTY_OAUTH_HOST
    // in include/agentty/io/http.hpp.  Without this, OAuth refresh dies on
    // getaddrinfo the moment the access token expires mid-session.
    if (const auto& ov = http::agentty_oauth_host_override(); ov.active()) {
        hreq.dial_host = ov.host;
        hreq.dial_port = ov.port;
    }
    hreq.path   = parsed->path;
    hreq.headers = {
        {"content-type", "application/x-www-form-urlencoded"},
        {"accept",       "application/json"},
        {"user-agent",   "agentty/" AGENTTY_VERSION},
    };
    hreq.body = form_urlencode(fields);
    // OAuth token exchange/refresh response is a tiny JSON object
    // (access_token, refresh_token, expires_in, ~1 KB). Tighten the
    // unary body cap so a runaway/misbehaving OAuth proxy can't stream
    // gigabytes back at us before we notice.
    hreq.max_body_bytes = 1ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client().send(hreq, tos);
    if (!resp) { r.transport_error = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Token exchange / refresh — the only place HTTP/JSON failure shapes get
// mapped to the typed OAuthErrorKind. Downstream callers see only
// TokenResult and dispatch on `kind`.
// ---------------------------------------------------------------------------

static TokenResult parse_token_json(const std::string& body, int http_status) {
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::BadResponse,
            std::string{"json parse failed: "} + e.what()});
    }
    if (http_status >= 400) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::ApiError,
            j.value("error_description",
                j.value("error",
                    std::string{"HTTP "} + std::to_string(http_status)))});
    }
    OAuthToken tok;
    tok.access_token  = j.value("access_token", "");
    tok.refresh_token = j.value("refresh_token", "");
    tok.expires_in_s  = j.value("expires_in", std::int64_t{0});
    if (tok.access_token.empty()) {
        return std::unexpected(OAuthError{
            OAuthErrorKind::MissingToken,
            "200 OK but no access_token in response"});
    }
    return tok;
}

std::string oauth_authorize_url(const PkceVerifier& verifier,
                                const OAuthState&   state) {
    std::string challenge = code_challenge_s256(verifier.value);
    std::ostringstream url;
    url << OAuthConfig::authorize_url
        << "?response_type=code"
        << "&client_id="             << OAuthConfig::client_id
        << "&redirect_uri="          << url_escape(OAuthConfig::redirect_uri)
        << "&scope="                 << url_escape(OAuthConfig::scopes)
        << "&state="                 << url_escape(state.value)
        << "&code_challenge="        << url_escape(challenge)
        << "&code_challenge_method=S256"
        << "&code=true";
    return url.str();
}

TokenResult exchange_code(const OAuthCode& code,
                          const PkceVerifier& verifier,
                          const OAuthState& state) {
    // Claude's callback returns "<code>#<state>" joined. Split into the two
    // halves: everything before '#' is the authorization code, everything
    // after is the state the IdP echoed back.
    std::string actual_code = code.value;
    std::string returned_state;
    if (auto hash = actual_code.find('#'); hash != std::string::npos) {
        returned_state = actual_code.substr(hash + 1);
        actual_code    = actual_code.substr(0, hash);
    }

    // Anti-CSRF (SECURITY_AUDIT): verify the echoed state matches the one we
    // generated for THIS login. A mismatch means the pasted code belongs to a
    // different authorization request (CSRF / mix-up / stale paste) — refuse
    // to exchange it. Constant-time compare so we don't leak the expected
    // value through timing. We only enforce when the IdP actually echoed a
    // state (returned_state non-empty); an older callback page that omits it
    // still relies on PKCE binding the code to the verifier.
    if (!returned_state.empty()) {
        const std::string& expected = state.value;
        bool ok = returned_state.size() == expected.size();
        unsigned char diff = ok ? 0u : 1u;
        for (size_t i = 0; i < expected.size() && i < returned_state.size(); ++i)
            diff |= static_cast<unsigned char>(expected[i] ^ returned_state[i]);
        if (!ok || diff != 0) {
            return std::unexpected(OAuthError{
                OAuthErrorKind::BadResponse,
                "state mismatch — the pasted code does not match this login "
                "request (possible CSRF or a stale/foreign code). Re-run login."});
        }
    }

    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "authorization_code"},
        {"code",          actual_code},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  OAuthConfig::redirect_uri},
        {"code_verifier", verifier.value},
        {"state",         state.value},
    });
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{
            OAuthErrorKind::Network, r.transport_error});
    return parse_token_json(r.body, r.status);
}

TokenResult refresh_access_token(const RefreshToken& refresh_token) {
    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "refresh_token"},
        {"client_id",     OAuthConfig::client_id},
        {"refresh_token", refresh_token.value},
    });
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{
            OAuthErrorKind::Network, r.transport_error});
    return parse_token_json(r.body, r.status);
}

// ---------------------------------------------------------------------------
// Resolve on startup
// ---------------------------------------------------------------------------

// Single-shot mailbox: written by resolve() on the main thread before the
// TUI is up, read by AgenttyApp::init() on the same main thread once
// maya::run has started. Single-producer / single-consumer, so a plain
// std::optional is sufficient — no atomics needed.
namespace {
std::optional<std::string> g_pending_refresh{};
}

void set_pending_refresh(std::string refresh_token) {
    g_pending_refresh = std::move(refresh_token);
}

std::optional<std::string> take_pending_refresh() {
    auto out = std::move(g_pending_refresh);
    g_pending_refresh.reset();
    return out;
}

Credentials resolve(const std::string& cli_api_key) {
    if (!cli_api_key.empty())
        return Credentials{cred::ApiKey{cli_api_key}};
    using namespace agentty::util;
    if (const char* k = env::get_or_null<env::Var::AnthropicApiKey>())
        return Credentials{cred::ApiKey{std::string{k}}};
    if (const char* t = env::get_or_null<env::Var::ClaudeOAuthToken>())
        return Credentials{cred::OAuth{std::string{t}, "", 0}};

    auto loaded = load_credentials();
    if (!loaded) return Credentials{cred::None{}};

    // Non-blocking: expired-with-refresh stashes the refresh token for
    // the reducer to pick up after the TUI is mounted; the network
    // round trip happens off the startup path. Expired-without-refresh
    // returns None so init.cpp opens the login modal.
    return std::visit([&](auto v) -> Credentials {
        using T = std::decay_t<decltype(v)>;
        if constexpr (!std::same_as<T, cred::OAuth>) {
            return Credentials{std::move(v)};
        } else {
            const bool expired = v.expires_at_ms != 0
                              && now_ms() >= v.expires_at_ms;
            if (!expired) return Credentials{std::move(v)};
            if (v.refresh_token.empty()) {
                // Nothing to refresh with; the in-TUI login modal will
                // surface the empty-creds state to the user.
                return Credentials{cred::None{}};
            }
            // Hand the refresh token off to the reducer's init Cmd.
            // The expired access_token is returned so anyone who
            // inspects creds (e.g. the auth_header in Deps) sees the
            // pre-refresh state until the background task lands;
            // submit_message gates user sends on the
            // oauth_refresh_in_flight flag so no request is fired with
            // a known-stale token.
            set_pending_refresh(v.refresh_token);
            return Credentials{std::move(v)};
        }
    }, *loaded);
}

// ---------------------------------------------------------------------------
// Synchronous fresh credential (worker-thread safe)
// ---------------------------------------------------------------------------

AuthHeader fresh_auth_header(const AuthHeader& fallback) {
    using namespace agentty::util;

    // CLI/env API keys are static — nothing to refresh. The env OAuth token
    // (CLAUDE_CODE_OAUTH_TOKEN) is opaque with no refresh token, so also
    // returned as-is.
    if (env::get_or_null<env::Var::AnthropicApiKey>()
     || env::get_or_null<env::Var::ClaudeOAuthToken>())
        return fallback;

    // Serialize refreshes: several subagent worker threads can land here at
    // once when a captured OAuth token expires. The first to win the lock
    // refreshes + persists; the rest re-read the freshly-saved token below.
    static std::mutex g_refresh_mu;
    std::scoped_lock lock(g_refresh_mu);

    auto loaded = load_credentials();
    if (!loaded) return fallback;

    auto* oauth = std::get_if<cred::OAuth>(&*loaded);
    if (!oauth) {
        // A saved API key: hand back its header (also covers the case where
        // creds changed on disk since startup).
        return make_auth_header(*loaded);
    }

    // Refresh a bit early (60s skew) so a token that expires mid-request
    // doesn't 401. No expiry info (expires_at_ms == 0) → trust it as-is.
    const bool stale = oauth->expires_at_ms != 0
                    && now_ms() >= oauth->expires_at_ms - 60'000;
    if (!stale || oauth->refresh_token.empty())
        return make_auth_header(*loaded);

    auto tr = refresh_access_token(RefreshToken{oauth->refresh_token});
    if (!tr) {
        // Refresh failed (network / revoked). Return whatever we have; the
        // transport will surface the eventual 401 with the login hint.
        return make_auth_header(*loaded);
    }

    Credentials refreshed{cred::OAuth{
        std::move(tr->access_token),
        tr->refresh_token.empty() ? oauth->refresh_token
                                  : std::move(tr->refresh_token),
        tr->expires_in_s ? now_ms() + tr->expires_in_s * 1000 : 0,
    }};
    save_credentials(refreshed);   // best-effort; the in-memory copy is authoritative
    return make_auth_header(refreshed);
}

// ---------------------------------------------------------------------------
// Browser launch
// ---------------------------------------------------------------------------

void open_browser(const std::string& url) {
#ifdef _WIN32
    // ShellExecuteA passes the URL as a single argument to the shell's URL
    // handler — no command line is composed, so there is no shell-quoting
    // surface. Safe as-is.
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // Launch the platform opener with the URL as a distinct argv element,
    // NOT interpolated into a `/bin/sh -c` string. This removes the shell
    // entirely from the code path: the URL can never be word-split, globbed,
    // command-substituted, or break out of quoting no matter what bytes it
    // carries. (The OAuth URL is all url-escaped today, so this is
    // defense-in-depth — but it keeps a future caller who passes an
    // arbitrary URL here safe by construction rather than by luck.)
#  if defined(__APPLE__)
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    // posix_spawn needs mutable char* argv; the strings outlive the call.
    std::string url_copy = url;
    char* argv[] = {const_cast<char*>(opener),
                    url_copy.data(),
                    nullptr};

    pid_t pid = 0;
    int   rc  = 0;

#if AGENTTY_HAVE_POSIX_SPAWN
    // Detach stdio from the browser process so it can't scribble on the
    // terminal agentty owns: redirect all three fds to /dev/null.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,  "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    rc = ::posix_spawnp(&pid, opener, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
#else
    // Fallback (Bionic without <spawn.h> below API 28): fork + exec with the
    // same stdio detachment done in the child before exec.
    pid = ::fork();
    if (pid == 0) {
        int rd = ::open("/dev/null", O_RDONLY);
        int wr = ::open("/dev/null", O_WRONLY);
        if (rd >= 0) { ::dup2(rd, STDIN_FILENO);  ::close(rd); }
        if (wr >= 0) { ::dup2(wr, STDOUT_FILENO); ::dup2(wr, STDERR_FILENO); ::close(wr); }
        ::execvp(opener, argv);
        ::_exit(127);   // exec only returns on failure
    } else if (pid < 0) {
        rc = errno;
    }
#endif

    // Reap the opener so it doesn't linger as a zombie. `open`/`xdg-open`
    // fork the real browser and exit promptly, so this wait is short; the
    // browser itself is reparented to init and keeps running. WNOHANG is
    // avoided intentionally — a blocking wait here is bounded by the
    // opener's own quick exit, and login is already a synchronous prompt.
    if (rc == 0 && pid > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
#endif
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

int cmd_login() {
    std::cout << "agentty — sign in (bring your own model)\n\n"
              << "  1) Paste an API key (Anthropic sk-ant-..., or any provider)\n"
              << "  2) OAuth via claude.ai (Pro/Max subscription)\n"
              << "\nChoice [1/2]: " << std::flush;
    std::string choice;
    std::getline(std::cin, choice);
    for (auto& c : choice) c = (char)std::tolower((unsigned char)c);

    // Default (empty / "1" / "api" / "key") is the API-key path; OAuth is
    // the explicit "2"/"oauth" opt-in. Dispatch is on the string, not on
    // positional order, so the displayed numbering above is presentation-only.
    const bool want_oauth = (choice == "2" || choice == "oauth"
                             || choice == "claude");
    if (!want_oauth) {
        std::cout << "\nPaste API key: " << std::flush;
        std::string key;
        std::getline(std::cin, key);
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                                || key.back() == ' ')) key.pop_back();
        if (key.empty()) { std::cerr << "No key entered.\n"; return 1; }
        Credentials c{cred::ApiKey{std::move(key)}};
        if (!save_credentials(c)) {
            std::cerr << "Failed to save credentials.\n"; return 1;
        }
        std::cout << "Saved API key to " << credentials_path().string() << "\n";
        return 0;
    }

    // OAuth PKCE flow
    PkceVerifier verifier{random_urlsafe(128)};
    OAuthState   state{random_urlsafe(32)};
    std::string  auth_url = oauth_authorize_url(verifier, state);

    std::cout << "\nOpening browser to authorize agentty...\n"
              << auth_url << "\n\n";
    open_browser(auth_url);

    std::cout << "After logging in, paste the code shown on the callback page: "
              << std::flush;
    std::string code_raw;
    std::getline(std::cin, code_raw);
    while (!code_raw.empty() && (code_raw.back() == '\r' || code_raw.back() == '\n'
                                 || code_raw.back() == ' ')) code_raw.pop_back();
    if (code_raw.empty()) { std::cerr << "No code entered.\n"; return 1; }

    auto tr = exchange_code(OAuthCode{std::move(code_raw)}, verifier, state);
    if (!tr) {
        std::cerr << "Token exchange failed: " << tr.error().render() << "\n";
        return 1;
    }
    Credentials c{cred::OAuth{
        std::move(tr->access_token),
        std::move(tr->refresh_token),
        tr->expires_in_s ? now_ms() + tr->expires_in_s * 1000 : 0
    }};
    if (!save_credentials(c)) {
        std::cerr << "Failed to save credentials.\n"; return 1;
    }
    std::cout << "\n\xE2\x9C\x93 Logged in. Saved to " << credentials_path().string() << "\n";
    return 0;
}

int cmd_logout() {
    auto p = credentials_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::cout << "No saved credentials.\n"; return 0;
    }
    if (!clear_credentials()) {
        std::cerr << "Failed to remove " << p.string() << "\n"; return 1;
    }
    std::cout << "Removed " << p.string() << "\n";
    return 0;
}

int cmd_status() {
    std::cout << "Credentials file: " << credentials_path().string() << "\n";
    using namespace agentty::util;
    if (env::get_or_null<env::Var::AnthropicApiKey>()) {
        std::cout << env::name<env::Var::AnthropicApiKey>()
                  << ": set (will be used, overrides file)\n";
    }
    if (env::get_or_null<env::Var::ClaudeOAuthToken>()) {
        std::cout << env::name<env::Var::ClaudeOAuthToken>()
                  << ": set (OAuth via env)\n";
    }
    auto loaded = load_credentials();
    if (!loaded) { std::cout << "Saved credentials: (none)\n"; return 0; }
    std::cout << "Saved method: " << persist_tag(*loaded) << "\n";
    if (auto* o = std::get_if<cred::OAuth>(&*loaded)) {
        if (o->expires_at_ms) {
            auto remaining_s = (o->expires_at_ms - now_ms()) / 1000;
            if (remaining_s <= 0) std::cout << "Token: expired\n";
            else std::cout << "Token expires in " << remaining_s << "s\n";
        } else {
            std::cout << "Token: no expiration info\n";
        }
        std::cout << "Refresh token: "
                  << (o->refresh_token.empty() ? "(none)" : "present") << "\n";
    }
    // At-rest encryption mode for the credentials file.
    std::cout << "At-rest encryption: aes-256-gcm ("
              << (crypt::passphrase_active()
                    ? "machine + passphrase"
                    : "machine-bound; set AGENTTY_PASSPHRASE or "
                      "AGENTTY_ENCRYPT_PASSPHRASE=1 to add a passphrase")
              << ")\n";
    // OS keystore backing (opt-in via AGENTTY_USE_KEYSTORE).
    std::cout << "OS keystore: " << keystore::backend_name();
    if (!keystore::available())
        std::cout << " (set AGENTTY_USE_KEYSTORE=1 to enable)";
    std::cout << "\n";
    return 0;
}

} // namespace agentty::auth
