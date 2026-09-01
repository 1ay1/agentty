// copilot_oauth.cpp — GitHub Copilot device-flow login + proxy-token exchange.
//
// See copilot_oauth.hpp. Structure mirrors chatgpt/codex_oauth.cpp; the flow is
// simpler (GitHub's plain device flow: no PKCE, no loopback callback server).
//
// Constants and quirks are cross-referenced against VS Code's own
// vscode-copilot-chat token manager and the failure reports of a dozen
// third-party integrations (see docs/design/copilot-provider.md):
//   • CLIENT_ID MUST be VS Code's GitHub App id — the exchange only accepts
//     App (ghu_) tokens, and the server keys its MODEL ALLOWLIST to this id.
//   • Route inference to the token response's `endpoints.api`, never a
//     hardcoded host (Individual/Business/Enterprise differ; guessing 404s).
//   • Editor-Version + Copilot-Integration-Id headers are mandatory on every
//     call — but those live on the transport Endpoint, not here.

#include "agentty/provider/copilot/copilot_oauth.hpp"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/util/base64.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

#ifndef _WIN32
#  include <sys/stat.h>   // chmod
#endif

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::provider::copilot {
namespace {
using json = nlohmann::json;
using auth::OAuthError;
using auth::OAuthErrorKind;
namespace fs = std::filesystem;

// ── Verified constants (docs/design/copilot-provider.md §2) ───────────────
constexpr const char* kClientId    = "Iv1.b507a08c87ecfe98";   // VS Code GitHub App
constexpr const char* kGithubHost  = "github.com";
constexpr const char* kApiHost     = "api.github.com";
constexpr const char* kDevicePath  = "/login/device/code";
constexpr const char* kTokenPath   = "/login/oauth/access_token";
constexpr const char* kExchangePath= "/copilot_internal/v2/token";
constexpr const char* kScope       = "read:user";
constexpr const char* kGrantType   = "urn:ietf:params:oauth:grant-type:device_code";
constexpr const char* kEditorVer   = "vscode/1.104.3";
constexpr const char* kPluginVer   = "copilot-chat/0.26.7";
constexpr const char* kUserAgent   = "GitHubCopilotChat/0.26.7";
constexpr const char* kIntegration = "vscode-chat";
// Fallback inference base when the token response omits endpoints.api.
constexpr const char* kDefaultApi  = "https://api.githubcopilot.com";

std::int64_t now_ms_impl() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    static const char* hex = "0123456789ABCDEF";
    auto esc = [](std::string_view s) {
        std::string out;
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
             || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
                out.push_back(static_cast<char>(c));
            else { out.push_back('%'); out.push_back(hex[(c>>4)&0xF]); out.push_back(hex[c&0xF]); }
        }
        return out;
    };
    std::string out;
    for (std::size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += esc(kv[i].first); out += '='; out += esc(kv[i].second);
    }
    return out;
}

struct HttpResult { int status = 0; std::string body; std::string transport_error; };

// One HTTPS request. `host` is github.com (device/token) or api.github.com
// (exchange). GitHub wants Accept: application/json to get JSON not form bodies.
HttpResult request(http::HttpMethod method, std::string_view host,
                   std::string_view path,
                   std::vector<std::pair<std::string, std::string>> headers,
                   std::string body = {}) {
    HttpResult r;
    http::Request req;
    req.method = method;
    req.host   = std::string{host};
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"accept",       "application/json"},
        {"user-agent",   kUserAgent},
        {"editor-version", kEditorVer},
        {"editor-plugin-version", kPluginVer},
    };
    for (auto& h : headers) req.headers.push_back({h.first, h.second});
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

// ── Persistence ───────────────────────────────────────────────────────────
std::mutex& store_mutex() { static std::mutex m; return m; }

struct Stored {
    GithubToken github;
    // Cached proxy token (best-effort; re-exchanged if missing/stale).
    CopilotToken proxy;
};

fs::path creds_path() { return auth::config_dir() / "copilot_credentials.json"; }

std::optional<Stored> load_unlocked() {
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
        Stored s;
        s.github.access_token = j.value("github_token", std::string{});
        s.github.token_type   = j.value("token_type", std::string{"bearer"});
        s.github.scope        = j.value("scope", std::string{});
        if (s.github.access_token.empty()) return std::nullopt;
        if (j.contains("proxy") && j["proxy"].is_object()) {
            const auto& p = j["proxy"];
            s.proxy.token         = p.value("token", std::string{});
            s.proxy.expires_at_ms = p.value("expires_at_ms", std::int64_t{0});
            s.proxy.endpoint_api  = p.value("endpoint_api", std::string{});
            s.proxy.sku           = p.value("sku", std::string{});
            s.proxy.chat_enabled  = p.value("chat_enabled", true);
        }
        return s;
    } catch (...) { return std::nullopt; }
}

bool save_unlocked(const Stored& s) {
    json j;
    j["github_token"] = s.github.access_token;
    j["token_type"]   = s.github.token_type;
    j["scope"]        = s.github.scope;
    if (s.proxy.valid()) {
        j["proxy"] = {
            {"token",         s.proxy.token},
            {"expires_at_ms", s.proxy.expires_at_ms},
            {"endpoint_api",  s.proxy.endpoint_api},
            {"sku",           s.proxy.sku},
            {"chat_enabled",  s.proxy.chat_enabled},
        };
    }
    std::string payload = j.dump();
    std::string to_write = payload;
    if (auto sealed = auth::crypt::seal(payload)) to_write = std::move(*sealed);

    std::error_code ec;
    fs::create_directories(creds_path().parent_path(), ec);
    fs::path tmp = creds_path();
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(to_write.data(), static_cast<std::streamsize>(to_write.size()));
        if (!ofs) return false;
    }
#ifndef _WIN32
    ::chmod(tmp.c_str(), 0600);
#endif
    fs::rename(tmp, creds_path(), ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// ── Device flow ─────────────────────────────────────────────────────────────
std::expected<DeviceCode, OAuthError> request_device_code(std::string& out_device_code,
                                                          int& out_interval) {
    auto r = request(http::HttpMethod::Post, kGithubHost, kDevicePath,
        {{"content-type", "application/x-www-form-urlencoded"}},
        form_encode({{"client_id", kClientId}, {"scope", kScope}}));
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status >= 400)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "GitHub device-code request failed (HTTP " + std::to_string(r.status) + ")"});
    try {
        auto j = json::parse(r.body);
        DeviceCode dc;
        dc.verification_uri = j.value("verification_uri", "https://github.com/login/device");
        dc.user_code        = j.value("user_code", std::string{});
        dc.expires_in       = j.value("expires_in", 900);
        out_device_code     = j.value("device_code", std::string{});
        out_interval        = j.value("interval", 5);
        if (dc.user_code.empty() || out_device_code.empty())
            return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                "incomplete device-code response"});
        return dc;
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse, e.what()});
    }
}

std::expected<GithubToken, OAuthError>
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
        auto r = request(http::HttpMethod::Post, kGithubHost, kTokenPath,
            {{"content-type", "application/x-www-form-urlencoded"}},
            form_encode({{"client_id", kClientId},
                         {"device_code", device_code},
                         {"grant_type", kGrantType}}));
        if (!r.transport_error.empty())
            return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
        try {
            auto j = json::parse(r.body);
            if (j.contains("access_token")) {
                GithubToken t;
                t.access_token = j.value("access_token", std::string{});
                t.token_type   = j.value("token_type", std::string{"bearer"});
                t.scope        = j.value("scope", std::string{});
                if (t.access_token.empty())
                    return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
                        "empty access_token"});
                return t;
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
            if (!err.empty())
                return std::unexpected(OAuthError{OAuthErrorKind::ApiError, err});
        } catch (...) { /* transient parse blip — keep polling */ }
    }
    return std::unexpected(OAuthError{OAuthErrorKind::Network,
        "device-code login timed out; start sign-in again for a new code"});
}

// ── Token exchange (ghu_ → proxy token) ─────────────────────────────────────
std::expected<CopilotToken, OAuthError> exchange(const GithubToken& gh) {
    // NOTE: GitHub wants `Authorization: token <ghu_>` here (NOT "Bearer").
    auto r = request(http::HttpMethod::Get, kApiHost, kExchangePath,
        {{"authorization", "token " + gh.access_token},
         {"copilot-integration-id", kIntegration}});
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status == 401 || r.status == 403)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "GitHub rejected the Copilot token exchange (HTTP " + std::to_string(r.status)
            + ") — your GitHub account may not have a Copilot subscription, "
              "or the sign-in was revoked. Re-run `agentty login`."});
    if (r.status == 404)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "Copilot token exchange returned 404 — this account has no Copilot "
            "entitlement on this endpoint."});
    if (r.status >= 400)
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "Copilot token exchange failed (HTTP " + std::to_string(r.status) + ")"});
    auto tok = parse_token_envelope(r.body, now_ms_impl());
    if (!tok)
        return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
            "no token in Copilot exchange response"});
    return *tok;
}

std::atomic<bool> g_force_refresh{false};

} // namespace

std::int64_t CopilotToken::now_ms() noexcept { return now_ms_impl(); }

std::optional<CopilotToken>
parse_token_envelope(std::string_view json_body, std::int64_t now_ms) {
    try {
        auto j = json::parse(json_body);
        CopilotToken t;
        t.token = j.value("token", std::string{});
        if (t.token.empty()) return std::nullopt;
        // Skew-safe LOCAL expiry: prefer refresh_in (VS Code's own strategy)
        // and add slack; fall back to expires_at; else a conservative default.
        if (const auto refresh_in = j.value("refresh_in", std::int64_t{0}); refresh_in > 0)
            t.expires_at_ms = now_ms + (refresh_in + 60) * 1000;
        else if (const auto exp = j.value("expires_at", std::int64_t{0}); exp > 0)
            t.expires_at_ms = exp * 1000;
        else
            t.expires_at_ms = now_ms + 25 * 60 * 1000;
        // Authoritative inference host — Individual/Business/Enterprise differ.
        if (j.contains("endpoints") && j["endpoints"].is_object())
            t.endpoint_api = j["endpoints"].value("api", std::string{});
        if (t.endpoint_api.empty()) t.endpoint_api = kDefaultApi;
        t.sku          = j.value("sku", std::string{});
        t.chat_enabled = j.value("chat_enabled", true);
        if (j.contains("limited_user_quotas") && j["limited_user_quotas"].is_object())
            t.quota_exhausted = j["limited_user_quotas"].value("chat", 1) == 0;
        return t;
    } catch (...) { return std::nullopt; }
}

// ── Public API ──────────────────────────────────────────────────────────────
fs::path credentials_path() { return creds_path(); }

std::optional<GithubToken> load_github_token() {
    std::scoped_lock lk(store_mutex());
    auto s = load_unlocked();
    if (!s) return std::nullopt;
    return s->github;
}

bool save_github_token(const GithubToken& gh) {
    std::scoped_lock lk(store_mutex());
    Stored s;
    if (auto cur = load_unlocked()) s = *cur;
    s.github = gh;
    s.proxy = {};   // a new sign-in invalidates any cached proxy token
    return save_unlocked(s);
}

bool clear_credentials() {
    std::scoped_lock lk(store_mutex());
    std::error_code ec;
    return fs::remove(creds_path(), ec) || !ec;
}

bool signed_in() {
    // Called by the provider-picker VIEW once per rendered frame. The naive
    // implementation (read file → unseal → JSON parse) costs disk + AES/KDF
    // work per frame while the picker is open. Cache the boolean keyed on
    // the file's (mtime, size): a stat is ~1µs and invalidates correctly on
    // sign-in, sign-out, and cross-process credential changes alike.
    std::scoped_lock lk(store_mutex());
    static bool cached = false;
    static std::filesystem::file_time_type cached_mtime{};
    static std::uintmax_t cached_size = static_cast<std::uintmax_t>(-1);
    std::error_code ec;
    const auto p = creds_path();
    const auto mtime = fs::last_write_time(p, ec);
    const auto size  = ec ? 0 : fs::file_size(p, ec);
    if (ec) {   // missing/unreadable → signed out; remember that cheaply
        cached = false;
        cached_mtime = {};
        cached_size = static_cast<std::uintmax_t>(-1);
        return false;
    }
    if (mtime != cached_mtime || size != cached_size) {
        auto s = load_unlocked();
        cached = s && !s->github.access_token.empty();
        cached_mtime = mtime;
        cached_size  = size;
    }
    return cached;
}

void invalidate_cached_token() { g_force_refresh.store(true); }

// ── Per-account model-support learning ─────────────────────────────────
namespace {
std::mutex& support_mu() { static std::mutex m; return m; }
fs::path support_path() { return auth::config_dir() / "copilot_model_support.json"; }

struct SupportSets { std::set<std::string> unsupported, supported; };

SupportSets load_support() {
    SupportSets s;
    std::ifstream ifs(support_path());
    if (!ifs) return s;
    try {
        auto j = json::parse(std::string{std::istreambuf_iterator<char>(ifs),
                                          std::istreambuf_iterator<char>()});
        for (auto& e : j.value("unsupported", json::array()))
            if (e.is_string()) s.unsupported.insert(e.get<std::string>());
        for (auto& e : j.value("supported", json::array()))
            if (e.is_string()) s.supported.insert(e.get<std::string>());
    } catch (const std::exception& e) {
        util::dbglog("copilot.support_cache.parse", e.what());
    } catch (...) {}
    return s;
}

void save_support(const SupportSets& s) {
    json j;
    j["unsupported"] = json::array();
    for (auto& id : s.unsupported) j["unsupported"].push_back(id);
    j["supported"] = json::array();
    for (auto& id : s.supported) j["supported"].push_back(id);
    std::error_code ec;
    fs::create_directories(support_path().parent_path(), ec);
    std::ofstream ofs(support_path(), std::ios::trunc);
    if (ofs) ofs << j.dump();
}
} // namespace

void note_unsupported_model(const std::string& model_id) {
    if (model_id.empty()) return;
    std::scoped_lock lk(support_mu());
    auto s = load_support();
    s.supported.erase(model_id);
    if (s.unsupported.insert(model_id).second) save_support(s);
}
bool is_unsupported_model(const std::string& model_id) {
    std::scoped_lock lk(support_mu());
    return load_support().unsupported.count(model_id) > 0;
}
void note_supported_model(const std::string& model_id) {
    if (model_id.empty()) return;
    std::scoped_lock lk(support_mu());
    auto s = load_support();
    bool changed = s.unsupported.erase(model_id) > 0;
    changed |= s.supported.insert(model_id).second;
    if (changed) save_support(s);
}
bool is_supported_model(const std::string& model_id) {
    std::scoped_lock lk(support_mu());
    return load_support().supported.count(model_id) > 0;
}

std::expected<GithubToken, OAuthError>
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

    // Persist the durable GitHub token; the proxy token is exchanged lazily on
    // first use via fresh_token().
    save_github_token(*tok);
    return *tok;
}

std::optional<CopilotToken> fresh_token() {
    // Serialize refreshes without holding store_mutex during the network call.
    static std::mutex refresh_mu;
    std::scoped_lock refresh_lk(refresh_mu);

    std::optional<Stored> loaded;
    {
        std::scoped_lock lk(store_mutex());
        loaded = load_unlocked();
    }
    if (!loaded || loaded->github.access_token.empty()) return std::nullopt;

    const bool force = g_force_refresh.exchange(false);
    if (!force && loaded->proxy.valid()
        && !loaded->proxy.expired(/*skew_ms=*/5 * 60 * 1000))
        return loaded->proxy;

    // Cross-process de-dup: one instance exchanges at a time, then losers
    // re-read the peer's freshly-saved token.
    auth::CrossProcessFileLock xlock(creds_path());
    if (xlock.held() && !force) {
        std::scoped_lock lk(store_mutex());
        if (auto disk = load_unlocked();
            disk && disk->proxy.valid()
            && !disk->proxy.expired(/*skew_ms=*/5 * 60 * 1000)) {
            return disk->proxy;   // a peer refreshed while we waited
        }
    }

    auto exchanged = exchange(loaded->github);
    if (!exchanged) return std::nullopt;

    std::scoped_lock lk(store_mutex());
    Stored s = *loaded;
    if (auto cur = load_unlocked()) s.github = cur->github;   // adopt newest ghu_
    s.proxy = *exchanged;
    save_unlocked(s);
    return *exchanged;
}

Entitlement account_entitlement() {
    // Short in-process cache: the picker may call this repeatedly while open.
    static std::mutex mu;
    static Entitlement cached;
    static std::int64_t fetched_at = 0;
    {
        std::scoped_lock lk(mu);
        if (cached.known && now_ms_impl() - fetched_at < 5 * 60 * 1000)
            return cached;
    }
    Entitlement e;   // known=false by default → permissive fallback
    auto gh = load_github_token();
    if (!gh) return e;

    // GET api.github.com/copilot_internal/user with the ghu_ token — the
    // authoritative per-account plan + quota snapshot.
    auto r = request(http::HttpMethod::Get, kApiHost, "/copilot_internal/user",
        {{"authorization", "token " + gh->access_token},
         {"copilot-integration-id", kIntegration}});
    if (!r.transport_error.empty() || r.status < 200 || r.status >= 300)
        return e;
    try {
        auto j = json::parse(r.body);
        e.plan         = j.value("copilot_plan", "");
        e.sku          = j.value("access_type_sku", "");
        e.chat_enabled = j.value("chat_enabled", true);
        // Premium models (Claude/Gemini/GPT-5.x/o-series) draw from the
        // premium_interactions quota. entitlement==0 && !unlimited && no quota
        // ⇒ this account can't run them (free tier). Any positive entitlement,
        // unlimited, or permitted overage ⇒ premium available.
        e.premium_available = true;
        if (j.contains("quota_snapshots") && j["quota_snapshots"].is_object()) {
            const auto& qs = j["quota_snapshots"];
            if (qs.contains("premium_interactions") && qs["premium_interactions"].is_object()) {
                const auto& p = qs["premium_interactions"];
                const bool unlimited = p.value("unlimited", false);
                const double ent     = p.value("entitlement", 0.0);
                const bool overage   = p.value("overage_permitted", false);
                e.premium_available = unlimited || ent > 0.0 || overage;
            }
        }
        e.known = true;
    } catch (...) { return Entitlement{}; }

    std::scoped_lock lk(mu);
    cached = e;
    fetched_at = now_ms_impl();
    return e;
}

namespace {
// Pull the `exp` (unix seconds) claim out of a JWT without verifying it (the
// server signs it; we only need the expiry to know when to refresh).
std::int64_t jwt_exp_ms(const std::string& jwt) {
    auto a = jwt.find('.');
    if (a == std::string::npos) return 0;
    auto b = jwt.find('.', a + 1);
    if (b == std::string::npos) return 0;
    std::string payload = jwt.substr(a + 1, b - a - 1);
    for (auto& c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (payload.size() % 4) payload.push_back('=');
    try {
        auto raw = util::base64_decode(payload);
        auto j = json::parse(raw);
        if (auto e = j.value("exp", std::int64_t{0}); e > 0) return e * 1000;
    } catch (const std::exception& e) {
        util::dbglog("copilot.jwt_exp.parse", e.what());
    } catch (...) {}
    return 0;
}

std::mutex& auto_mu() { static std::mutex m; return m; }
AutoSession& auto_cache() { static AutoSession s; return s; }
} // namespace

// Guarded by auto_mu(), like auto_cache(). 0 = no backoff armed.
std::int64_t& auto_failed_until_ms() { static std::int64_t v = 0; return v; }

std::optional<AutoSession> auto_session() {
    std::scoped_lock lk(auto_mu());
    if (auto_cache().valid()) return auto_cache();

    // NEGATIVE CACHE. Everything below is blocking network I/O held under
    // auto_mu(), and a FAILED attempt used to cache nothing — so an account
    // that can't open an Auto session (no entitlement, org policy, a flaky
    // 5xx) re-dialled /models/session on EVERY turn, and every turn paid a
    // full TLS handshake + round-trip before the first token. That is the
    // "weirdly hangs for a second" report.
    //
    // Back off instead: remember the last failure and answer nullopt straight
    // from memory for a short window. Long enough that a turn never waits on a
    // known-bad endpoint, short enough that a user who fixes their plan or
    // waits out a blip recovers without restarting. invalidate_auto_session()
    // clears it, so an explicit account switch or a 401 retry re-probes at once.
    const auto now = CopilotToken::now_ms();
    if (now < auto_failed_until_ms()) return std::nullopt;
    constexpr std::int64_t kBackoffMs = 60'000;
    // Any early return below is a failure; arm the backoff up front so no
    // path can forget it.
    auto_failed_until_ms() = now + kBackoffMs;

    auto tok = fresh_token();
    if (!tok || !tok->chat_enabled) return std::nullopt;

    std::string host = tok->endpoint_api;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    if (auto slash = host.find('/'); slash != std::string::npos) host = host.substr(0, slash);

    // POST {api}/models/session with the CAPI api-version — the server returns
    // the per-account available models + a signed session token.
    auto r = request(http::HttpMethod::Post, host, "/models/session",
        {{"authorization", "Bearer " + tok->token},
         {"copilot-integration-id", kIntegration},
         {"x-github-api-version", kAutoApiVersion},
         {"content-type", "application/json"}},
        R"({"auto_mode":{"model_hints":["auto"]}})");
    if (!r.transport_error.empty() || r.status < 200 || r.status >= 300)
        return std::nullopt;
    try {
        auto j = json::parse(r.body);
        AutoSession s;
        s.session_token  = j.value("session_token", "");
        s.selected_model = j.value("selected_model", "");
        s.endpoint_api   = tok->endpoint_api;
        if (j.contains("available_models") && j["available_models"].is_array())
            for (auto& m : j["available_models"])
                if (m.is_string()) s.available_models.push_back(m.get<std::string>());
        if (s.session_token.empty()) return std::nullopt;
        s.expires_at_ms = jwt_exp_ms(s.session_token);
        auto_cache() = s;
        auto_failed_until_ms() = 0;   // succeeded — disarm the backoff
        return s;
    } catch (...) { return std::nullopt; }
}

void invalidate_auto_session() {
    std::scoped_lock lk(auto_mu());
    auto_cache() = AutoSession{};
    // Also clear the negative cache: an explicit invalidation (account switch,
    // 401 retry) is a statement that the world changed, so re-probe at once
    // rather than serving a stale "this account can't" for another minute.
    auto_failed_until_ms() = 0;
}

} // namespace agentty::provider::copilot
