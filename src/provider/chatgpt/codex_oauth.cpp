#include "agentty/provider/chatgpt/codex_oauth.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/io/http.hpp"
#include "agentty/util/base64.hpp"

namespace agentty::provider::chatgpt {
namespace {
using json = nlohmann::json;
using auth::OAuthError;
using auth::OAuthErrorKind;
namespace fs = std::filesystem;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// RFC-3986 unreserved passthrough, everything else %HH.
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

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (std::size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += url_escape(kv[i].first);
        out += '=';
        out += url_escape(kv[i].second);
    }
    return out;
}

// A single POST to auth.openai.com. `json_body` chooses application/json vs
// x-www-form-urlencoded (the Codex refresh uses JSON, the code+key exchanges
// use form). Returns {status, body} or a transport error string.
struct PostResult {
    int status = 0;
    std::string body;
    std::string transport_error;
};

PostResult post(std::string_view path, std::string body, bool json_body) {
    PostResult r;
    http::Request req;
    req.method = http::HttpMethod::Post;
    req.host   = "auth.openai.com";
    req.port   = 443;
    req.path   = std::string{path};
    req.headers = {
        {"content-type", json_body ? "application/json"
                                   : "application/x-www-form-urlencoded"},
        {"accept",       "application/json"},
        {"user-agent",   "agentty/" AGENTTY_VERSION},
    };
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

// Pull one claim out of a JWT (middle segment, base64url). Best-effort: any
// malformed input returns empty so the caller degrades to no account_id.
std::string jwt_claim(const std::string& jwt, const char* key) {
    const auto a = jwt.find('.');
    if (a == std::string::npos) return {};
    const auto b = jwt.find('.', a + 1);
    if (b == std::string::npos) return {};
    std::string payload = jwt.substr(a + 1, b - a - 1);
    // base64url → standard base64 for the decoder.
    for (auto& c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (payload.size() % 4) payload.push_back('=');
    try {
        auto raw = util::base64_decode(payload);
        auto j = json::parse(raw);
        // account id can live directly or under auth.chatgpt_account_id.
        if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
        if (j.contains("https://api.openai.com/auth")) {
            const auto& o = j["https://api.openai.com/auth"];
            if (o.contains(key) && o[key].is_string()) return o[key].get<std::string>();
        }
    } catch (...) {}
    return {};
}

// Parse {access_token,refresh_token,id_token,expires_in} into a credential.
std::expected<CodexCredentials, OAuthError>
parse_tokens(const std::string& body, int status,
             const std::string& prior_refresh) {
    json j;
    try { j = json::parse(body); }
    catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            std::string{"json parse failed: "} + e.what()});
    }
    if (status >= 400) {
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            j.value("error_description",
                j.value("error", std::string{"HTTP "} + std::to_string(status)))});
    }
    CodexCredentials c;
    c.access_token  = j.value("access_token", std::string{});
    c.refresh_token = j.value("refresh_token", prior_refresh);
    c.id_token      = j.value("id_token", std::string{});
    if (c.access_token.empty()) {
        return std::unexpected(OAuthError{OAuthErrorKind::MissingToken,
            "no access_token in token response"});
    }
    if (const auto secs = j.value("expires_in", std::int64_t{0}); secs > 0)
        c.expires_at_ms = now_ms() + secs * 1000;
    if (!c.id_token.empty())
        c.account_id = jwt_claim(c.id_token, "chatgpt_account_id");
    return c;
}

// RFC-8693 token-exchange: id_token → usable OpenAI api key. Best-effort;
// login still succeeds (with just the bearer token) if this fails.
std::string mint_api_key(const std::string& id_token) {
    if (id_token.empty()) return {};
    auto r = post("/oauth/token", form_encode({
        {"grant_type",         OAuthConfig::token_exchange_grant},
        {"client_id",          OAuthConfig::client_id},
        {"requested_token",    OAuthConfig::requested_token_apikey},
        {"subject_token",      id_token},
        {"subject_token_type", OAuthConfig::subject_token_type_idtoken},
    }), /*json_body=*/false);
    if (!r.transport_error.empty() || r.status >= 400) return {};
    try {
        return json::parse(r.body).value("access_token", std::string{});
    } catch (...) { return {}; }
}

struct DeviceAuthorization {
    std::string device_auth_id;
    std::string user_code;
    int interval_s = 5;
};

struct DeviceGrant {
    std::string authorization_code;
    std::string code_verifier;
    std::string code_challenge;
};

std::string response_error(const PostResult& r, std::string fallback) {
    try {
        const auto j = json::parse(r.body);
        return j.value("error_description", j.value("error", std::move(fallback)));
    } catch (...) {
        return fallback;
    }
}

std::expected<DeviceAuthorization, OAuthError> request_device_authorization() {
    auto r = post("/api/accounts/deviceauth/usercode",
                  json{{"client_id", OAuthConfig::client_id}}.dump(),
                  /*json_body=*/true);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});
    if (r.status == 404) {
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "device-code login is disabled for this ChatGPT account or workspace; "
            "enable it in ChatGPT security settings, or reconnect with "
            "`ssh -L 1455:localhost:1455 ...`"});
    }
    if (r.status >= 400) {
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            response_error(r, "device-code request failed (HTTP "
                              + std::to_string(r.status) + ")")});
    }

    try {
        const auto j = json::parse(r.body);
        DeviceAuthorization d;
        d.device_auth_id = j.value("device_auth_id", std::string{});
        d.user_code = j.value("user_code", j.value("usercode", std::string{}));
        if (const auto it = j.find("interval"); it != j.end()) {
            if (it->is_number_integer()) d.interval_s = it->get<int>();
            else if (it->is_string()) d.interval_s = std::stoi(it->get<std::string>());
        }
        d.interval_s = std::clamp(d.interval_s, 1, 30);
        if (d.device_auth_id.empty() || d.device_auth_id.size() > 1024
            || d.user_code.empty() || d.user_code.size() > 128) {
            return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                "device-code response omitted a valid id or one-time code"});
        }
        return d;
    } catch (const std::exception& e) {
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            std::string{"invalid device-code response: "} + e.what()});
    }
}

std::expected<DeviceGrant, OAuthError>
poll_device_authorization(const DeviceAuthorization& device, int timeout_s,
                          const CodexCancelProbe& cancelled) {
    using clock = std::chrono::steady_clock;
    const auto timeout = std::chrono::seconds{std::max(timeout_s, 1)};
    const auto deadline = clock::now() + timeout;
    const auto was_cancelled = [&] { return cancelled && cancelled(); };
    const auto wait_for_next_poll = [&] {
        const auto wake = std::min(deadline, clock::now()
            + std::chrono::seconds{device.interval_s});
        while (clock::now() < wake && !was_cancelled()) {
            std::this_thread::sleep_for(std::min(
                std::chrono::milliseconds{100},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    wake - clock::now())));
        }
    };

    while (clock::now() < deadline) {
        if (was_cancelled()) {
            return std::unexpected(OAuthError{OAuthErrorKind::Network,
                "device-code login cancelled"});
        }
        auto r = post("/api/accounts/deviceauth/token", json{
            {"device_auth_id", device.device_auth_id},
            {"user_code", device.user_code},
        }.dump(), /*json_body=*/true);
        if (!r.transport_error.empty()) {
            // A device authorization remains valid across transient network
            // failures. Retry at the server cadence until its deadline.
            wait_for_next_poll();
            continue;
        }

        if (r.status >= 200 && r.status < 300) {
            try {
                const auto j = json::parse(r.body);
                DeviceGrant grant{
                    .authorization_code = j.value("authorization_code", std::string{}),
                    .code_verifier = j.value("code_verifier", std::string{}),
                    .code_challenge = j.value("code_challenge", std::string{}),
                };
                if (grant.authorization_code.empty()
                    || grant.authorization_code.size() > 8192
                    || grant.code_verifier.size() < 43
                    || grant.code_verifier.size() > 128
                    || grant.code_challenge.empty()
                    || grant.code_challenge.size() > 128
                    || auth::code_challenge_s256(grant.code_verifier)
                        != grant.code_challenge) {
                    return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                        "device approval returned invalid PKCE parameters"});
                }
                return grant;
            } catch (const std::exception& e) {
                return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                    std::string{"invalid device approval response: "} + e.what()});
            }
        }

        // OpenAI currently uses 403/404 for "authorization pending".
        if (r.status != 403 && r.status != 404) {
            return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
                response_error(r, "device-code polling failed (HTTP "
                                  + std::to_string(r.status) + ")")});
        }
        wait_for_next_poll();
    }
    return std::unexpected(OAuthError{OAuthErrorKind::Network,
        "device-code login timed out; start sign-in again for a new code"});
}

// ── Loopback callback server (blocking, single connection) ─────────────────
//
// OpenAI pins the redirect to http://localhost:1455/auth/callback, so we bind
// that port, accept one request, extract ?code=&state=, and reply with a
// browser-friendly page. Returns the raw query on success. Cross-platform via
// Winsock / BSD sockets; no framework needed for a one-shot local exchange.
struct CallbackResult {
    std::string code;
    std::string state;
    std::string error;   // non-empty on failure (bind/accept/timeout)
};

std::string query_param(std::string_view target, std::string_view key) {
    // target is the request-target, e.g. /auth/callback?code=X&state=Y
    const auto q = target.find('?');
    if (q == std::string_view::npos) return {};
    std::string_view qs = target.substr(q + 1);
    std::string want{key};
    want += '=';
    std::size_t pos = 0;
    while (pos < qs.size()) {
        auto amp = qs.find('&', pos);
        std::string_view pair = qs.substr(pos, amp == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : amp - pos);
        if (pair.substr(0, want.size()) == want) {
            std::string_view raw = pair.substr(want.size());
            // Minimal %XX / '+' decode — sufficient for OAuth code/state.
            std::string out;
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) { out.push_back(char(hi * 16 + lo)); i += 2; continue; }
                }
                out.push_back(raw[i] == '+' ? ' ' : raw[i]);
            }
            return out;
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return {};
}

CallbackResult wait_for_callback(int timeout_s,
                                 const CodexCancelProbe& cancelled) {
    CallbackResult out;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        out.error = "WSAStartup failed"; return out;
    }
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } wsa_guard;
    using sock_t = SOCKET;
    const sock_t kInvalid = INVALID_SOCKET;
    auto close_sock = [](sock_t s) { closesocket(s); };
#else
    using sock_t = int;
    const sock_t kInvalid = -1;
    auto close_sock = [](sock_t s) { ::close(s); };
#endif

    sock_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == kInvalid) { out.error = "cannot create callback socket"; return out; }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // bind 127.0.0.1 only
    addr.sin_port = htons(static_cast<uint16_t>(OAuthConfig::callback_port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(srv);
        out.error = "port 1455 is busy — close any running `codex`/Codex login "
                    "and retry";
        return out;
    }
    if (::listen(srv, 1) != 0) { close_sock(srv); out.error = "listen failed"; return out; }

    // Poll accept with a deadline so a user who abandons the browser doesn't
    // hang the process forever.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_s);
    for (;;) {
        const bool cancel = cancelled && cancelled();
        if (cancel || std::chrono::steady_clock::now() >= deadline) {
            close_sock(srv);
            out.error = cancel ? "login cancelled" : "login timed out";
            return out;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        timeval tv{1, 0};   // 1s slices so cancel/timeout stay responsive
        const int ready = ::select(static_cast<int>(srv + 1), &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        sock_t cli = ::accept(srv, nullptr, nullptr);
        if (cli == kInvalid) continue;

        // Read through short select() slices so a local peer that connects and
        // stalls cannot pin port 1455 or defeat cancellation/the deadline.
        std::string req;
        char buf[4096];
        while (req.size() <= 16384) {
            const bool read_cancel = cancelled && cancelled();
            if (read_cancel || std::chrono::steady_clock::now() >= deadline) {
                close_sock(cli);
                close_sock(srv);
                out.error = read_cancel ? "login cancelled" : "login timed out";
                return out;
            }
            fd_set cli_rfds;
            FD_ZERO(&cli_rfds);
            FD_SET(cli, &cli_rfds);
            timeval cli_tv{1, 0};
            const int readable = ::select(static_cast<int>(cli + 1), &cli_rfds,
                                          nullptr, nullptr, &cli_tv);
            if (readable < 0) break;
            if (readable == 0) continue;
#if defined(_WIN32)
            int n = ::recv(cli, buf, sizeof(buf), 0);
#else
            auto n = ::recv(cli, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos) break;
        }

        // Parse "GET /auth/callback?code=...&state=... HTTP/1.1".
        std::string target;
        if (const auto sp = req.find(' '); sp != std::string::npos) {
            const auto sp2 = req.find(' ', sp + 1);
            if (sp2 != std::string::npos)
                target = req.substr(sp + 1, sp2 - sp - 1);
        }
        out.code  = query_param(target, "code");
        out.state = query_param(target, "state");
        const std::string err = query_param(target, "error");

        const bool ok = !out.code.empty() && err.empty();
        const std::string page = ok
            ? "<html><head><title>agentty</title></head><body "
              "style='font-family:system-ui;background:#0b0d10;color:#e6e6e6;"
              "display:flex;align-items:center;justify-content:center;height:100vh'>"
              "<div style='text-align:center'><h2>&#10003; Signed in to ChatGPT</h2>"
              "<p>You can close this tab and return to agentty.</p></div></body></html>"
            : "<html><body style='font-family:system-ui'><h2>Sign-in failed</h2>"
              "<p>Return to agentty and try again.</p></body></html>";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: " + std::to_string(page.size())
                         + "\r\nConnection: close\r\n\r\n" + page;
        (void)::send(cli, resp.data(), static_cast<int>(resp.size()), 0);
        close_sock(cli);
        close_sock(srv);
        if (!ok) out.error = err.empty() ? "no authorization code in callback"
                                         : ("authorization denied: " + err);
        return out;
    }
}

// ── Credential (de)serialization ───────────────────────────────────────────
json to_json(const CodexCredentials& c) {
    return json{
        {"access_token",  c.access_token},
        {"refresh_token", c.refresh_token},
        {"id_token",      c.id_token},
        {"account_id",    c.account_id},
        {"api_key",       c.api_key},
        {"expires_at_ms", c.expires_at_ms},
    };
}

CodexCredentials from_json(const json& j) {
    CodexCredentials c;
    c.access_token  = j.value("access_token", std::string{});
    c.refresh_token = j.value("refresh_token", std::string{});
    c.id_token      = j.value("id_token", std::string{});
    c.account_id    = j.value("account_id", std::string{});
    c.api_key       = j.value("api_key", std::string{});
    c.expires_at_ms = j.value("expires_at_ms", std::int64_t{0});
    return c;
}

std::mutex& store_mutex() {
    static std::mutex m;
    return m;
}

std::uint64_t& store_generation() {
    static std::uint64_t generation = 0; // guarded by store_mutex()
    return generation;
}

std::optional<CodexCredentials> load_codex_credentials_unlocked() {
    std::ifstream ifs(auth::config_dir() / "codex_credentials.json",
                      std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string raw{std::istreambuf_iterator<char>(ifs), {}};
    if (raw.empty()) return std::nullopt;

    std::string plain = raw;
    if (auth::crypt::looks_sealed(raw)) {
        auto pt = auth::crypt::unseal(raw);
        if (!pt) return std::nullopt;
        plain = std::move(*pt);
    }
    try {
        auto c = from_json(json::parse(plain));
        if (c.access_token.empty()) return std::nullopt;
        return c;
    } catch (...) { return std::nullopt; }
}

bool save_codex_credentials_unlocked(const CodexCredentials& c) {
    const auto path = auth::config_dir() / "codex_credentials.json";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const std::string payload = to_json(c).dump();
    std::string to_write;
    if (auto sealed = auth::crypt::seal(payload)) to_write = std::move(*sealed);
    else return false;

    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(to_write.data(), static_cast<std::streamsize>(to_write.size()));
        if (!ofs) return false;
    }
#if !defined(_WIN32)
    ::chmod(tmp.c_str(), 0600);
#endif
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

} // namespace

bool CodexCredentials::expired(std::int64_t skew_ms) const noexcept {
    return expires_at_ms != 0 && now_ms() >= expires_at_ms - skew_ms;
}

fs::path codex_credentials_path() {
    return auth::config_dir() / "codex_credentials.json";
}

std::optional<CodexCredentials> load_codex_credentials() {
    std::scoped_lock lock{store_mutex()};
    return load_codex_credentials_unlocked();
}

bool save_codex_credentials(const CodexCredentials& c) {
    std::scoped_lock lock{store_mutex()};
    if (!save_codex_credentials_unlocked(c)) return false;
    ++store_generation();
    return true;
}

bool clear_codex_credentials() {
    std::scoped_lock lock{store_mutex()};
    std::error_code ec;
    const bool ok = fs::remove(codex_credentials_path(), ec) || !ec;
    if (ok) ++store_generation();
    return ok;
}

std::string codex_authorize_url(const auth::PkceVerifier& verifier,
                                const auth::OAuthState&   state) {
    const std::string challenge = auth::code_challenge_s256(verifier.value);
    std::ostringstream url;
    url << OAuthConfig::authorize_url
        << "?response_type=code"
        << "&client_id="              << OAuthConfig::client_id
        << "&redirect_uri="           << url_escape(OAuthConfig::redirect_uri)
        << "&scope="                  << url_escape(OAuthConfig::scopes)
        << "&code_challenge="         << url_escape(challenge)
        << "&code_challenge_method=S256"
        << "&id_token_add_organizations=true"
        << "&codex_cli_simplified_flow=true"
        << "&state="                  << url_escape(state.value)
        << "&originator="             << url_escape(OAuthConfig::originator);
    return url.str();
}

std::expected<CodexCredentials, OAuthError>
codex_exchange_code(const auth::OAuthCode& code, const auth::PkceVerifier& verifier) {
    auto r = post("/oauth/token", form_encode({
        {"grant_type",    "authorization_code"},
        {"code",          code.value},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  OAuthConfig::redirect_uri},
        {"code_verifier", verifier.value},
    }), /*json_body=*/false);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, "");
    if (!c) return c;
    c->api_key = mint_api_key(c->id_token);
    return c;
}

std::expected<CodexCredentials, OAuthError>
codex_refresh(const CodexCredentials& current) {
    if (current.refresh_token.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::ApiError,
            "no refresh_token — sign in again"});
    // Codex refresh uses a JSON body (not form-encoded).
    const std::string body = json{
        {"client_id",     OAuthConfig::client_id},
        {"grant_type",    "refresh_token"},
        {"refresh_token", current.refresh_token},
    }.dump();
    auto r = post("/oauth/token", body, /*json_body=*/true);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, current.refresh_token);
    if (!c) return c;
    if (c->account_id.empty()) c->account_id = current.account_id;
    c->api_key = mint_api_key(c->id_token.empty() ? current.id_token : c->id_token);
    if (c->api_key.empty()) c->api_key = current.api_key;   // keep the old key on mint failure
    return c;
}

bool codex_device_auth_preferred() noexcept {
    if (const char* value = std::getenv("AGENTTY_CHATGPT_DEVICE_AUTH")) {
        const std::string_view setting{value};
        if (setting == "0" || setting == "false" || setting == "no"
            || setting == "off") return false;
        if (setting == "1" || setting == "true" || setting == "yes"
            || setting == "on") return true;
    }
    const auto present = [](const char* name) {
        const char* value = std::getenv(name);
        return value && *value;
    };
    return present("SSH_CONNECTION") || present("SSH_CLIENT") || present("SSH_TTY");
}

std::expected<CodexCredentials, OAuthError>
codex_device_login(CodexDeviceCodeSink on_device_code, int timeout_s,
                   CodexCancelProbe cancelled) {
    if (cancelled && cancelled()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "device-code login cancelled"});
    }
    auto device = request_device_authorization();
    if (!device) return std::unexpected(device.error());

    if (on_device_code) {
        on_device_code(CodexDeviceCode{
            .verification_url = std::string{OAuthConfig::issuer} + "/codex/device",
            .user_code = device->user_code,
        });
    }

    auto grant = poll_device_authorization(*device, timeout_s, cancelled);
    if (!grant) return std::unexpected(grant.error());
    if (cancelled && cancelled()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "device-code login cancelled"});
    }

    const std::string redirect_uri = std::string{OAuthConfig::issuer}
                                   + "/deviceauth/callback";
    auto r = post("/oauth/token", form_encode({
        {"grant_type",    "authorization_code"},
        {"code",          grant->authorization_code},
        {"client_id",     OAuthConfig::client_id},
        {"redirect_uri",  redirect_uri},
        {"code_verifier", grant->code_verifier},
    }), /*json_body=*/false);
    if (!r.transport_error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, r.transport_error});

    auto c = parse_tokens(r.body, r.status, "");
    if (!c) return c;
    c->api_key = mint_api_key(c->id_token);
    return c;
}

std::expected<CodexCredentials, OAuthError>
codex_login(int timeout_s, CodexDeviceCodeSink on_device_code,
            CodexCancelProbe cancelled) {
    if (codex_device_auth_preferred())
        return codex_device_login(std::move(on_device_code), timeout_s,
                                  std::move(cancelled));

    auth::PkceVerifier verifier{auth::random_urlsafe(64)};
    auth::OAuthState   state{auth::random_urlsafe(32)};

    const std::string url = codex_authorize_url(verifier, state);
    auth::open_browser(url);

    auto cb = wait_for_callback(timeout_s, cancelled);
    if (!cb.error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, cb.error});

    // Anti-CSRF: require the IdP to echo our state and compare it in constant
    // time. Missing state is a mismatch, never an optional compatibility path.
    const std::string& want = state.value;
    bool ok = cb.state.size() == want.size();
    unsigned char diff = ok ? 0 : 1;
    for (std::size_t i = 0; i < want.size() && i < cb.state.size(); ++i)
        diff |= static_cast<unsigned char>(want[i] ^ cb.state[i]);
    if (!ok || diff)
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            "state mismatch — re-run login"});

    if (cancelled && cancelled()) {
        return std::unexpected(OAuthError{OAuthErrorKind::Network,
            "login cancelled"});
    }
    auto c = codex_exchange_code(auth::OAuthCode{cb.code}, verifier);
    return c;
}

std::optional<CodexCredentials> codex_fresh_credentials() {
    // Serialize refresh network calls, but do not hold store_mutex while the
    // network is in flight: sign-out and a new login must remain responsive.
    static std::mutex refresh_mu;
    std::scoped_lock refresh_lock{refresh_mu};

    std::optional<CodexCredentials> loaded;
    std::uint64_t source_generation = 0;
    {
        std::scoped_lock store_lock{store_mutex()};
        loaded = load_codex_credentials_unlocked();
        source_generation = store_generation();
    }
    if (!loaded) return std::nullopt;
    if (!loaded->expired(/*skew_ms=*/60'000)) return loaded;
    if (loaded->refresh_token.empty()) return loaded;

    // Cross-process serialization + double-checked re-read: block every
    // agentty instance on one refresh at a time, then re-load so the losers
    // adopt the winner's freshly-saved token instead of each firing a
    // redundant refresh (ChatGPT/Codex also rotates refresh tokens, so
    // concurrent refreshes mutually invalidate and spin the refresh loop).
    auth::CrossProcessFileLock xlock(codex_credentials_path());
    if (xlock.held()) {
        std::optional<CodexCredentials> disk;
        {
            std::scoped_lock store_lock{store_mutex()};
            disk = load_codex_credentials_unlocked();
            source_generation = store_generation();
        }
        if (!disk) return loaded;
        if (!disk->expired(/*skew_ms=*/60'000)) return disk;  // a peer refreshed
        if (disk->refresh_token.empty()) return disk;
        loaded = std::move(disk);   // refresh with the freshest on-disk token
    }

    auto refreshed = codex_refresh(*loaded);
    if (!refreshed) return loaded;

    std::scoped_lock store_lock{store_mutex()};
    auto current = load_codex_credentials_unlocked();
    const bool source_unchanged = current
        && current->access_token == loaded->access_token
        && current->refresh_token == loaded->refresh_token
        && current->account_id == loaded->account_id;
    if (store_generation() != source_generation || !source_unchanged) {
        // Sign-out, account activation, another process, or a new login won
        // the race. Never resurrect/overwrite it from the stale snapshot.
        return current;
    }
    if (!save_codex_credentials_unlocked(*refreshed)) return loaded;
    ++store_generation();
    return *refreshed;
}

} // namespace agentty::provider::chatgpt
