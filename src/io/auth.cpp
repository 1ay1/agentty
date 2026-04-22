#include "moha/io/auth.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <atomic>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#ifdef _WIN32
#  include <io.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace moha::auth {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Credentials methods
// ---------------------------------------------------------------------------

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Credentials::is_expired() const {
    if (method != Method::OAuth) return false;
    if (expires_at_ms == 0) return false;
    return now_ms() >= expires_at_ms;
}

std::string Credentials::header_value() const {
    if (method == Method::OAuth) return std::string("Bearer ") + access_token;
    return access_token;
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
    fs::path p = base / "moha";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path credentials_path() { return config_dir() / "credentials.json"; }

// ---------------------------------------------------------------------------
// CURL share handle — one process-wide SSL session + DNS + connection cache.
// ---------------------------------------------------------------------------
// libcurl's default is per-easy caches; a fresh curl_easy_init pays TLS
// handshake + DNS for every request even when they go to the same host.
// CURLSH lets us opt into shared caches so turn N+1 reuses the TCP/TLS
// session of turn N to api.anthropic.com — saving ~80–120ms of handshake
// on every follow-up call.
//
// The share carries its own locks; we just hand libcurl a mutex per data
// kind. Built once lazily, never destroyed (process-lifetime).
namespace {

struct ShareBundle {
    CURLSH* share = nullptr;
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks {};
};

ShareBundle& share_bundle() {
    // Heap-allocate and leak: libcurl holds a pointer to `locks` via
    // CURLSHOPT_USERDATA, so the object must outlive every curl handle
    // that references the share. Process-lifetime leak is cheaper than
    // careful teardown across unknown destruction order.
    static ShareBundle* b = [] {
        auto* bb = new ShareBundle;
        bb->share = curl_share_init();
        if (!bb->share) return bb;
        curl_share_setopt(bb->share, CURLSHOPT_LOCKFUNC,
            +[](CURL*, curl_lock_data data, curl_lock_access, void* user) {
                auto* self = static_cast<ShareBundle*>(user);
                if (static_cast<int>(data) >= 0
                    && static_cast<int>(data) < CURL_LOCK_DATA_LAST)
                    self->locks[data].lock();
            });
        curl_share_setopt(bb->share, CURLSHOPT_UNLOCKFUNC,
            +[](CURL*, curl_lock_data data, void* user) {
                auto* self = static_cast<ShareBundle*>(user);
                if (static_cast<int>(data) >= 0
                    && static_cast<int>(data) < CURL_LOCK_DATA_LAST)
                    self->locks[data].unlock();
            });
        curl_share_setopt(bb->share, CURLSHOPT_USERDATA, bb);
        curl_share_setopt(bb->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(bb->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(bb->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        return bb;
    }();
    return *b;
}

} // namespace

void apply_shared_cache(void* handle) {
    CURL* curl = static_cast<CURL*>(handle);
    if (!curl) return;
    auto& b = share_bundle();
    if (b.share) curl_easy_setopt(curl, CURLOPT_SHARE, b.share);
    // Default DNS TTL is 60 s — wasteful when we hit one host (api.anthropic.com)
    // turn after turn. 10 minutes matches Anthropic's published edge stability
    // and lets a long agent session keep one resolved IP for its entire lifetime.
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    // Cap the global connection cache. We only ever talk to api.anthropic.com
    // and platform.claude.com, so the default 5 is plenty — but make it explicit
    // so an unrelated curl global default change can't bloat us.
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 8L);
}

// ---------------------------------------------------------------------------
// Pre-warm: open TCP+TLS to api.anthropic.com while the user is still typing,
// so the first real request skips ~150–300 ms of handshake. Runs detached.
// ---------------------------------------------------------------------------
void prewarm_anthropic() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;

    std::thread([]{
        CURL* curl = curl_easy_init();
        if (!curl) return;
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
#ifdef CURLOPT_TCP_FASTOPEN
        curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L);
#endif
        apply_tls_options(curl);
        apply_shared_cache(curl);
        // We intentionally ignore the return code — a failure here is harmless
        // (the real request on Enter will retry with proper error reporting).
        (void)curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }).detach();
}

void apply_tls_options(void* handle) {
    CURL* curl = static_cast<CURL*>(handle);
    if (!curl) return;
    if (const char* ca = std::getenv("CURL_CA_BUNDLE"); ca && *ca) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    } else if (const char* ca2 = std::getenv("SSL_CERT_FILE"); ca2 && *ca2) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca2);
    }
    // Pin TLS 1.3 minimum. Anthropic's edge supports 1.3 universally; locking
    // the floor here means cold handshake is 1-RTT instead of 1.2's 2-RTT, and
    // session-resumption (via the shared SSL_SESSION cache) gives 0-RTT on the
    // second turn. Saves ~80–150 ms on first byte for a fresh agent session.
    curl_easy_setopt(curl, CURLOPT_SSLVERSION,
                     (long)(CURL_SSLVERSION_TLSv1_3 | CURL_SSLVERSION_MAX_DEFAULT));
#ifdef _WIN32
    // Merge the Windows system cert store into OpenSSL's trust anchors.
    // Why: corporate environments push MITM root CAs via Group Policy into the
    // Windows store only; the bundled ca-certificates file won't contain them,
    // so TLS handshakes fail with CURLE_PEER_FAILED_VERIFICATION behind the proxy.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    if (const char* ins = std::getenv("MOHA_INSECURE"); ins && *ins
        && std::string(ins) != "0" && std::string(ins) != "false") {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // apply_tls_options is called per-handle; warn exactly once.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "warning: MOHA_INSECURE is set — TLS certificate verification is disabled. "
                "Auth credentials are exposed to MITM. Unset the variable to restore safety.\n");
        }
    }
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
// where it exists world-readable between open() and chmod().
// Windows: fall back to std::ofstream (ACLs are out of scope here).
static bool write_private(const fs::path& p, const std::string& content) {
#ifdef _WIN32
    std::ofstream ofs(p, std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), (std::streamsize)content.size());
    return static_cast<bool>(ofs);
#else
    int fd = ::open(p.c_str(),
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
            return false;
        }
        buf += n;
        remaining -= (size_t)n;
    }
    return ::close(fd) == 0;
#endif
}

std::optional<Credentials> load_credentials() {
    std::ifstream ifs(credentials_path());
    if (!ifs) return std::nullopt;
    try {
        json j; ifs >> j;
        Credentials c;
        auto m = j.value("method", "api_key");
        if (m == "oauth")   c.method = Method::OAuth;
        else if (m == "api_key") c.method = Method::ApiKey;
        else return std::nullopt;
        c.access_token  = j.value("access_token", "");
        c.refresh_token = j.value("refresh_token", "");
        c.expires_at_ms = j.value("expires_at", int64_t{0});
        if (!c.is_valid()) return std::nullopt;
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

bool save_credentials(const Credentials& c) {
    json j;
    j["method"] = (c.method == Method::OAuth) ? "oauth" : "api_key";
    j["access_token"] = c.access_token;
    j["refresh_token"] = c.refresh_token;
    j["expires_at"] = c.expires_at_ms;
    fs::path p = credentials_path();
    if (!write_private(p, j.dump(2))) return false;
    restrict_perms(p); // belt-and-suspenders for pre-existing files
    return true;
}

bool clear_credentials() {
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

std::string random_urlsafe(size_t n) {
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::random_device rd;
    std::mt19937_64 rng(((uint64_t)rd() << 32) ^ rd());
    std::uniform_int_distribution<int> dist(0, sizeof(charset) - 2);
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(charset[dist(rng)]);
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
// HTTP helpers
// ---------------------------------------------------------------------------

namespace {
size_t curl_collect_cb(char* p, size_t sz, size_t nm, void* u) {
    auto* s = static_cast<std::string*>(u);
    s->append(p, sz * nm);
    return sz * nm;
}

struct HttpResult { long http = 0; std::string body; CURLcode rc = CURLE_OK; };

static std::string form_urlencode(const std::vector<std::pair<std::string,std::string>>& kv,
                                   CURL* curl) {
    std::string out;
    for (size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        char* k = curl_easy_escape(curl, kv[i].first.c_str(), (int)kv[i].first.size());
        char* v = curl_easy_escape(curl, kv[i].second.c_str(), (int)kv[i].second.size());
        out += k ? k : ""; out += '='; out += v ? v : "";
        if (k) curl_free(k);
        if (v) curl_free(v);
    }
    return out;
}

HttpResult http_post_form(const std::string& url,
    const std::vector<std::pair<std::string,std::string>>& fields) {
    HttpResult r;
    CURL* curl = curl_easy_init();
    if (!curl) { r.rc = CURLE_FAILED_INIT; return r; }
    std::string body = form_urlencode(fields, curl);
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "content-type: application/x-www-form-urlencoded");
    hdr = curl_slist_append(hdr, "accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_collect_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    apply_tls_options(curl);
    apply_shared_cache(curl);
    r.rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.http);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return r;
}
} // namespace

// ---------------------------------------------------------------------------
// Token exchange / refresh
// ---------------------------------------------------------------------------

static TokenResponse parse_token_json(const std::string& body, long http) {
    TokenResponse r;
    try {
        auto j = json::parse(body);
        if (http >= 400) {
            r.error = j.value("error_description",
                       j.value("error", std::string("HTTP ") + std::to_string(http)));
            return r;
        }
        r.access_token  = j.value("access_token", "");
        r.refresh_token = j.value("refresh_token", "");
        r.expires_in_s  = j.value("expires_in", int64_t{0});
        r.ok = !r.access_token.empty();
        if (!r.ok) r.error = "no access_token in response";
    } catch (const std::exception& e) {
        r.error = std::string("parse failed: ") + e.what();
    }
    return r;
}

TokenResponse exchange_code(const std::string& code,
                            const std::string& verifier,
                            const std::string& state) {
    // Claude's callback often returns "<code>#<state>" joined. Split if present.
    std::string actual_code = code;
    auto hash = actual_code.find('#');
    if (hash != std::string::npos) actual_code = actual_code.substr(0, hash);

    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "authorization_code"},
        {"code",          actual_code},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  OAuthConfig::redirect_uri},
        {"code_verifier", verifier},
        {"state",         state},
    });
    if (r.rc != CURLE_OK) {
        TokenResponse t; t.error = curl_easy_strerror(r.rc); return t;
    }
    return parse_token_json(r.body, r.http);
}

TokenResponse refresh_access_token(const std::string& refresh_token) {
    auto r = http_post_form(OAuthConfig::token_url, {
        {"grant_type",    "refresh_token"},
        {"client_id",     OAuthConfig::client_id},
        {"refresh_token", refresh_token},
    });
    if (r.rc != CURLE_OK) {
        TokenResponse t; t.error = curl_easy_strerror(r.rc); return t;
    }
    return parse_token_json(r.body, r.http);
}

// ---------------------------------------------------------------------------
// Resolve on startup
// ---------------------------------------------------------------------------

Credentials resolve(const std::string& cli_api_key) {
    // 1. CLI flag
    if (!cli_api_key.empty()) {
        return {Method::ApiKey, cli_api_key, "", 0};
    }
    // 2. ANTHROPIC_API_KEY
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {
        return {Method::ApiKey, k, "", 0};
    }
    // 3. CLAUDE_CODE_OAUTH_TOKEN
    if (const char* t = std::getenv("CLAUDE_CODE_OAUTH_TOKEN"); t && *t) {
        return {Method::OAuth, t, "", 0};
    }
    // 4. credentials file
    auto loaded = load_credentials();
    if (!loaded) return {};
    Credentials c = *loaded;
    if (c.method == Method::OAuth && c.is_expired() && !c.refresh_token.empty()) {
        std::fprintf(stderr, "moha: refreshing OAuth token... ");
        auto tr = refresh_access_token(c.refresh_token);
        if (tr.ok) {
            c.access_token  = tr.access_token;
            if (!tr.refresh_token.empty()) c.refresh_token = tr.refresh_token;
            c.expires_at_ms = tr.expires_in_s
                ? now_ms() + tr.expires_in_s * 1000 : 0;
            save_credentials(c);
            std::fprintf(stderr, "ok\n");
        } else {
            std::fprintf(stderr, "FAILED: %s\n", tr.error.c_str());
            std::fprintf(stderr,
                "moha: stored OAuth token is expired and refresh failed.\n"
                "      run 'moha login' to re-authenticate.\n");
            return {}; // force caller to treat as not-authenticated
        }
    } else if (c.method == Method::OAuth && c.is_expired()) {
        std::fprintf(stderr,
            "moha: stored OAuth token is expired and no refresh token.\n"
            "      run 'moha login'.\n");
        return {};
    }
    return c;
}

// ---------------------------------------------------------------------------
// Browser launch
// ---------------------------------------------------------------------------

static void open_browser(const std::string& url) {
#ifdef _WIN32
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

static std::string url_encode(const std::string& s) {
    CURL* c = curl_easy_init();
    char* esc = curl_easy_escape(c, s.c_str(), (int)s.size());
    std::string out = esc ? esc : "";
    if (esc) curl_free(esc);
    curl_easy_cleanup(c);
    return out;
}

int cmd_login() {
    std::cout << "moha — authenticate with Claude\n\n"
              << "  1) OAuth via claude.ai (Pro/Max subscription)\n"
              << "  2) Paste an Anthropic API key (sk-ant-...)\n"
              << "\nChoice [1/2]: " << std::flush;
    std::string choice;
    std::getline(std::cin, choice);
    for (auto& c : choice) c = (char)std::tolower((unsigned char)c);

    if (choice == "2" || choice == "api" || choice == "key") {
        std::cout << "\nPaste API key: " << std::flush;
        std::string key;
        std::getline(std::cin, key);
        while (!key.empty() && (key.back() == '\r' || key.back() == '\n'
                                || key.back() == ' ')) key.pop_back();
        if (key.empty()) { std::cerr << "No key entered.\n"; return 1; }
        Credentials c{Method::ApiKey, key, "", 0};
        if (!save_credentials(c)) {
            std::cerr << "Failed to save credentials.\n"; return 1;
        }
        std::cout << "Saved API key to " << credentials_path().string() << "\n";
        return 0;
    }

    // OAuth PKCE flow
    std::string verifier  = random_urlsafe(128);
    std::string challenge = code_challenge_s256(verifier);
    std::string state     = random_urlsafe(32);

    std::ostringstream url;
    url << OAuthConfig::authorize_url
        << "?response_type=code"
        << "&client_id="             << OAuthConfig::client_id
        << "&redirect_uri="          << url_encode(OAuthConfig::redirect_uri)
        << "&scope="                 << url_encode(OAuthConfig::scopes)
        << "&state="                 << state
        << "&code_challenge="        << challenge
        << "&code_challenge_method=S256"
        << "&code=true";

    std::string auth_url = url.str();
    std::cout << "\nOpening browser to authorize moha...\n"
              << auth_url << "\n\n";
    open_browser(auth_url);

    std::cout << "After logging in, paste the code shown on the callback page: "
              << std::flush;
    std::string code;
    std::getline(std::cin, code);
    while (!code.empty() && (code.back() == '\r' || code.back() == '\n'
                             || code.back() == ' ')) code.pop_back();
    if (code.empty()) { std::cerr << "No code entered.\n"; return 1; }

    auto tr = exchange_code(code, verifier, state);
    if (!tr.ok) {
        std::cerr << "Token exchange failed: " << tr.error << "\n";
        return 1;
    }
    Credentials c;
    c.method = Method::OAuth;
    c.access_token  = tr.access_token;
    c.refresh_token = tr.refresh_token;
    c.expires_at_ms = tr.expires_in_s
        ? now_ms() + tr.expires_in_s * 1000 : 0;
    if (!save_credentials(c)) {
        std::cerr << "Failed to save credentials.\n"; return 1;
    }
    std::cout << "\n✓ Logged in. Saved to " << credentials_path().string() << "\n";
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
    if (const char* k = std::getenv("ANTHROPIC_API_KEY"); k && *k) {
        std::cout << "ANTHROPIC_API_KEY: set (will be used, overrides file)\n";
    }
    if (const char* t = std::getenv("CLAUDE_CODE_OAUTH_TOKEN"); t && *t) {
        std::cout << "CLAUDE_CODE_OAUTH_TOKEN: set (OAuth via env)\n";
    }
    auto c = load_credentials();
    if (!c) { std::cout << "Saved credentials: (none)\n"; return 0; }
    std::cout << "Saved method: "
              << (c->method == Method::OAuth ? "oauth" : "api_key") << "\n";
    if (c->method == Method::OAuth) {
        if (c->expires_at_ms) {
            auto remaining_s = (c->expires_at_ms - now_ms()) / 1000;
            if (remaining_s <= 0) std::cout << "Token: expired\n";
            else std::cout << "Token expires in " << remaining_s << "s\n";
        } else {
            std::cout << "Token: no expiration info\n";
        }
        std::cout << "Refresh token: "
                  << (c->refresh_token.empty() ? "(none)" : "present") << "\n";
    }
    return 0;
}

} // namespace moha::auth
