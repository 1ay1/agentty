// agentty::provider::kimi — Kimi Code OAuth (device flow). See kimi_oauth.hpp.
//
// The simpler sibling of copilot_oauth: Kimi's device-flow access_token is the
// API bearer directly (no proxy exchange), and it refreshes via the standard
// `refresh_token` grant. Three form-encoded POSTs to `auth.kimi.com`:
//   /api/oauth/device_authorization,
//   /api/oauth/token (grant_type=urn:ietf:params:oauth:grant-type:device_code),
//   /api/oauth/token (grant_type=refresh_token)

#include "agentty/provider/kimi/kimi_oauth.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/io/http.hpp"

namespace agentty::provider::kimi {
namespace {
using json = nlohmann::json;
using auth::OAuthError;
using auth::OAuthErrorKind;
namespace fs = std::filesystem;

// ── Verified constants (packages/oauth/src/constants.ts, e22479a6) ──────────
constexpr const char* kOAuthHost  = "auth.kimi.com";
constexpr const char* kClientId   = "17e5f671-d194-4dfb-9706-5516cb48c098";
constexpr const char* kDevicePath = "/api/oauth/device_authorization";
constexpr const char* kTokenPath  = "/api/oauth/token";
constexpr const char* kUserAgent  = "kimi_code_cli/" AGENTTY_VERSION;
// X-Msh-Platform: Kimi's server routes the device-flow (device-approval page,
// not a bare login) on this identifier. Must be exactly "kimi_code_cli".
constexpr const char* kMshPlatform = "kimi_code_cli";

std::int64_t now_ms_impl() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// ASCII-clean a header value (Kimi rejects non-ASCII; drop it, fall back).
std::string ascii_header(std::string_view v, const char* fallback = "unknown") {
    std::string out;
    for (unsigned char c : v) if (c >= 0x20 && c <= 0x7E) out.push_back(static_cast<char>(c));
    // trim
    while (!out.empty() && out.back() == ' ') out.pop_back();
    std::size_t b = out.find_first_not_of(' ');
    if (b != std::string::npos) out = out.substr(b);
    return out.empty() ? std::string{fallback} : out;
}

std::string host_name() {
    char buf[256] = {0};
#if defined(_WIN32)
    DWORD n = sizeof(buf);
    if (::GetComputerNameA(buf, &n)) return ascii_header(buf);
#else
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return ascii_header(buf);
#endif
    return "unknown";
}

std::string os_version() {
#if defined(_WIN32)
    return "Windows";
#else
    struct utsname u{};
    if (::uname(&u) == 0) return ascii_header(u.release);
    return "unknown";
#endif
}

std::string device_model() {
#if defined(_WIN32)
    return "Windows";
#else
    struct utsname u{};
    if (::uname(&u) == 0)
        return ascii_header(std::string{u.sysname} + " " + u.release + " " + u.machine);
    return "unknown";
#endif
}

// A stable per-machine device id (RFC 4122 v4 UUID), minted once and persisted
// next to the credentials so the same device is recognized across sessions.
std::string device_id() {
    const fs::path path = auth::config_dir() / "kimi_device_id";
    {
        std::ifstream ifs(path);
        if (ifs) {
            std::string id((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' '))
                id.pop_back();
            if (!id.empty()) return id;
        }
    }
    // Mint a v4 UUID.
    std::random_device rd;
    std::mt19937_64 gen(((std::uint64_t)rd() << 32) ^ rd() ^
                        (std::uint64_t)now_ms_impl());
    auto h = [&](int n) {
        std::string s;
        static const char* x = "0123456789abcdef";
        for (int i = 0; i < n; ++i) s.push_back(x[gen() & 0xF]);
        return s;
    };
    std::string id = h(8) + "-" + h(4) + "-4" + h(3) + "-" +
                     std::string(1, "89ab"[gen() & 0x3]) + h(3) + "-" + h(12);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs) ofs << id;
    return id;
}

// The X-Msh-* device-identity header block Kimi requires on every OAuth (and
// API) request. Without it the auth server treats the caller as a generic web
// client and serves a plain login page instead of the device-approval flow.
std::vector<std::pair<std::string, std::string>> device_headers_impl() {
    return {
        {"x-msh-platform",     kMshPlatform},
        {"x-msh-version",      AGENTTY_VERSION},
        {"x-msh-device-name",  host_name()},
        {"x-msh-device-model", device_model()},
        {"x-msh-os-version",   os_version()},
        {"x-msh-device-id",    device_id()},
    };
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    static const char* hex = "0123456789ABCDEF";
    auto esc = [](std::string_view s) {
        std::string out;
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
             || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    };
    std::string out;
    for (const auto& [k, v] : kv) {
        if (!out.empty()) out.push_back('&');
        out += esc(k);
        out.push_back('=');
        out += esc(v);
    }
    return out;
}

struct HttpResult { int status = 0; std::string body; std::string transport_error; };

// One HTTPS POST to the OAuth host, form-encoded body, JSON response.
HttpResult post_form(std::string_view path, std::string body) {
    HttpResult r;
    http::Request req;
    req.method = http::HttpMethod::Post;
    req.host   = kOAuthHost;
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"accept",       "application/json"},
        {"content-type", "application/x-www-form-urlencoded"},
        {"user-agent",   kUserAgent},
    };
    for (auto& h : device_headers_impl()) req.headers.push_back({h.first, h.second});
    if (const auto& ov = http::agentty_oauth_host_override(); ov.active()) {
        req.dial_host = ov.host;
        req.dial_port = ov.port;
    }
    req.body = std::move(body);
    req.max_body_bytes = 1ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(10'000);
    tos.total   = std::chrono::milliseconds(30'000);

    auto resp = http::default_client().send(req, tos);
    if (!resp) { r.transport_error = resp.error().render(); return r; }
    r.status = resp->status;
    r.body   = std::move(resp->body);
    return r;
}

// ── Credential store ────────────────────────────────────────────────────────
std::mutex& store_mutex() { static std::mutex m; return m; }

fs::path creds_path() { return auth::config_dir() / "kimi_credentials.json"; }

std::optional<KimiToken> load_unlocked() {
    std::ifstream ifs(creds_path(), std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string raw{std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    if (raw.empty()) return std::nullopt;
    if (auth::crypt::looks_sealed(raw)) {
        auto pt = auth::crypt::unseal(raw);
        if (!pt) return std::nullopt;
        raw = std::move(*pt);
    }
    try {
        auto j = json::parse(raw);
        KimiToken t;
        t.access_token  = j.value("access_token", std::string{});
        t.refresh_token = j.value("refresh_token", std::string{});
        t.expires_at_ms = j.value("expires_at_ms", std::int64_t{0});
        t.token_type    = j.value("token_type", std::string{"Bearer"});
        t.scope         = j.value("scope", std::string{});
        if (t.access_token.empty()) return std::nullopt;
        return t;
    } catch (...) { return std::nullopt; }
}

bool save_unlocked(const KimiToken& t) {
    json j;
    j["access_token"]  = t.access_token;
    j["refresh_token"] = t.refresh_token;
    j["expires_at_ms"] = t.expires_at_ms;
    j["token_type"]    = t.token_type;
    j["scope"]         = t.scope;

    std::string payload = j.dump();
    std::string to_write = payload;
    if (auto sealed = auth::crypt::seal(payload)) to_write = std::move(*sealed);

    std::error_code ec;
    fs::create_directories(creds_path().parent_path(), ec);
    std::ofstream ofs(creds_path(), std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(to_write.data(), static_cast<std::streamsize>(to_write.size()));
    return static_cast<bool>(ofs);
}

std::atomic<bool> g_force_refresh{false};

// ── Wire parsing ─────────────────────────────────────────────────────────────
std::optional<KimiToken> parse_token_body(std::string_view body, std::int64_t now_ms) {
    try {
        auto j = json::parse(body);
        const std::string access = j.value("access_token", std::string{});
        if (access.empty()) return std::nullopt;
        KimiToken t;
        t.access_token  = access;
        t.refresh_token = j.value("refresh_token", std::string{});
        t.token_type    = j.value("token_type", std::string{"Bearer"});
        t.scope         = j.value("scope", std::string{});
        // expires_in is server seconds; keep a small slack so we refresh before
        // the real expiry rather than eating a 401 mid-turn.
        const std::int64_t expires_in = j.value("expires_in", std::int64_t{0});
        t.expires_at_ms = expires_in > 0 ? now_ms + (expires_in - 60) * 1000 : 0;
        return t;
    } catch (...) { return std::nullopt; }
}

// ── Device flow ─────────────────────────────────────────────────────────────
std::expected<DeviceCode, OAuthError>
request_device_code(std::string& out_device_code, int& out_interval) {
    auto r = post_form(kDevicePath, form_encode({{"client_id", kClientId}}));
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status >= 400)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "Kimi device-code request failed (HTTP " + std::to_string(r.status) + ")"});
    try {
        auto j = json::parse(r.body);
        // The device-auth server still returns verification URLs on the
        // DEPRECATED www.kimi.com host, which now shows only a "Kimi has been
        // rebranded as Kimi.ai" dead-end page (no device-approval UI). The
        // working device page lives on www.kimi.ai. Rewrite the host so the
        // opened/displayed URL actually approves the device.
        auto to_kimi_ai = [](std::string u) {
            for (std::string_view from : {"https://www.kimi.com/", "https://kimi.com/"}) {
                if (u.rfind(from, 0) == 0)
                    return "https://www.kimi.ai/" + u.substr(from.size());
            }
            return u;
        };
        DeviceCode dc;
        dc.verification_uri = to_kimi_ai(j.value("verification_uri",
                                      std::string{"https://www.kimi.ai/code/authorize_device"}));
        dc.verification_uri_complete =
            to_kimi_ai(j.value("verification_uri_complete", dc.verification_uri));
        dc.user_code    = j.value("user_code", std::string{});
        dc.expires_in   = j.value("expires_in", 900);
        out_device_code = j.value("device_code", std::string{});
        out_interval    = j.value("interval", 5);
        if (dc.user_code.empty() || out_device_code.empty())
            return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                "incomplete device-code response"});
        return dc;
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse, e.what()});
    }
}

std::expected<KimiToken, OAuthError>
poll_for_token(const std::string& device_code, int interval_s, int timeout_s,
               const CancelProbe& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    int interval = interval_s > 0 ? interval_s : 5;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled && cancelled())
            return std::unexpected(OAuthError{OAuthErrorKind::Network, "login cancelled"});
        for (int slept = 0; slept < interval; ++slept) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (cancelled && cancelled())
                return std::unexpected(OAuthError{OAuthErrorKind::Network, "login cancelled"});
        }
        auto r = post_form(kTokenPath,
            form_encode({{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                         {"client_id", kClientId},
                         {"device_code", device_code}}));
        if (!r.transport_error.empty())
            return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
        try {
            auto j = json::parse(r.body);
            if (j.contains("access_token")) {
                if (auto t = parse_token_body(r.body, now_ms_impl())) return *t;
                return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
                    "empty access_token"});
            }
            const std::string err = j.value("error", std::string{});
            if (err == "authorization_pending") continue;         // keep waiting
            if (err == "slow_down") { interval += 5; continue; }  // back off
            if (err == "expired_token")
                return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
                    "the device code expired — start sign-in again for a new code"});
            if (err == "access_denied")
                return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
                    "sign-in was denied"});
            if (!err.empty()) {
                const std::string desc = j.value("error_description", std::string{});
                return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
                    desc.empty() ? err : err + ": " + desc});
            }
        } catch (...) { /* transient parse blip — keep polling */ }
    }
    return std::unexpected(OAuthError{OAuthErrorKind::Network,
        "device-code login timed out; start sign-in again for a new code"});
}

// ── refresh_token grant ──────────────────────────────────────────────────────
std::expected<KimiToken, OAuthError> refresh(const KimiToken& cur) {
    if (cur.refresh_token.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
            "no refresh token — sign in again"});
    auto r = post_form(kTokenPath,
        form_encode({{"grant_type", "refresh_token"},
                     {"client_id", kClientId},
                     {"refresh_token", cur.refresh_token}}));
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status == 401 || r.status == 403)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "Kimi rejected the token refresh (HTTP " + std::to_string(r.status)
            + ") — the sign-in may have been revoked. Re-run `agentty login`."});
    if (r.status >= 400)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "Kimi token refresh failed (HTTP " + std::to_string(r.status) + ")"});
    auto t = parse_token_body(r.body, now_ms_impl());
    if (!t)
        return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
            "no token in Kimi refresh response"});
    // Servers may omit refresh_token on refresh — keep the current one.
    if (t->refresh_token.empty()) t->refresh_token = cur.refresh_token;
    return *t;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────
std::int64_t KimiToken::now_ms() noexcept { return now_ms_impl(); }

std::vector<std::pair<std::string, std::string>> device_headers() {
    return device_headers_impl();
}

fs::path credentials_path() { return creds_path(); }

std::optional<KimiToken> load_token() {
    std::scoped_lock lk(store_mutex());
    return load_unlocked();
}

bool save_token(const KimiToken& tok) {
    std::scoped_lock lk(store_mutex());
    return save_unlocked(tok);
}

bool clear_credentials() {
    std::scoped_lock lk(store_mutex());
    std::error_code ec;
    return fs::remove(creds_path(), ec) || !ec;
}

bool signed_in() {
    // Called by the picker VIEW once per rendered frame — cache the boolean
    // keyed on the file's (mtime, size) so a fresh stat (~1µs) invalidates on
    // sign-in / sign-out / cross-process change without unseal+parse per frame.
    std::scoped_lock lk(store_mutex());
    static std::mutex cache_mu;
    static bool cached = false;
    static std::int64_t cached_mtime = -1;
    static std::uintmax_t cached_size = 0;
    std::scoped_lock clk(cache_mu);
    std::error_code ec;
    auto st = fs::last_write_time(creds_path(), ec);
    if (ec) { cached = false; cached_mtime = -1; cached_size = 0; return false; }
    auto sz = fs::file_size(creds_path(), ec);
    auto mt = st.time_since_epoch().count();
    if (mt == cached_mtime && sz == cached_size) return cached;
    cached = load_unlocked().has_value();
    cached_mtime = mt;
    cached_size = sz;
    return cached;
}

void invalidate_cached_token() { g_force_refresh.store(true); }

std::optional<KimiToken>
parse_token_response(std::string_view json_body, std::int64_t now_ms) {
    return parse_token_body(json_body, now_ms);
}

std::expected<KimiToken, OAuthError>
login(int timeout_s, DeviceCodeSink on_device_code, CancelProbe cancelled) {
    if (cancelled && cancelled())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, "login cancelled"});

    std::string device_code;
    int interval = 5;
    auto dc = request_device_code(device_code, interval);
    if (!dc) return std::unexpected(dc.error());
    if (on_device_code) on_device_code(*dc);

    auto tok = poll_for_token(device_code, interval, timeout_s, cancelled);
    if (!tok) return std::unexpected(tok.error());

    save_token(*tok);
    return *tok;
}

std::optional<KimiToken> fresh_token() {
    // Serialize refreshes without holding store_mutex across the network call.
    static std::mutex refresh_mu;
    std::scoped_lock lk(refresh_mu);

    auto cur = load_token();
    if (!cur) return std::nullopt;

    const bool force = g_force_refresh.exchange(false);
    if (!force && !cur->expired(30'000)) return cur;

    auto refreshed = refresh(*cur);
    if (!refreshed) {
        // If the token still looks valid (e.g. a transient refresh blip and no
        // forced invalidation), fall back to it rather than blocking the turn.
        if (!force && cur->valid() && !cur->expired()) return cur;
        return std::nullopt;
    }
    save_token(*refreshed);
    return *refreshed;
}

} // namespace agentty::provider::kimi
