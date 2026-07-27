#include "agentty/provider/chatgpt/codex_oauth.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
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
                                 const std::atomic<bool>& cancel) {
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
        if (cancel.load() || std::chrono::steady_clock::now() >= deadline) {
            close_sock(srv);
            out.error = cancel.load() ? "login cancelled" : "login timed out";
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

        // Read the request line (we only need the GET target).
        std::string req;
        char buf[4096];
        for (int i = 0; i < 8; ++i) {
#if defined(_WIN32)
            int n = ::recv(cli, buf, sizeof(buf), 0);
#else
            auto n = ::recv(cli, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos) break;
            if (req.size() > 16384) break;
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

} // namespace

bool CodexCredentials::expired(std::int64_t skew_ms) const noexcept {
    return expires_at_ms != 0 && now_ms() >= expires_at_ms - skew_ms;
}

fs::path codex_credentials_path() {
    return auth::config_dir() / "codex_credentials.json";
}

std::optional<CodexCredentials> load_codex_credentials() {
    std::scoped_lock lock{store_mutex()};
    std::ifstream ifs(codex_credentials_path(), std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string raw{std::istreambuf_iterator<char>(ifs), {}};
    if (raw.empty()) return std::nullopt;

    std::string plain = raw;
    if (auth::crypt::looks_sealed(raw)) {
        auto pt = auth::crypt::unseal(raw);
        if (!pt) return std::nullopt;   // tampered / wrong machine
        plain = std::move(*pt);
    }
    try {
        auto c = from_json(json::parse(plain));
        if (c.access_token.empty()) return std::nullopt;
        return c;
    } catch (...) { return std::nullopt; }
}

bool save_codex_credentials(const CodexCredentials& c) {
    std::scoped_lock lock{store_mutex()};
    const auto path = codex_credentials_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const std::string payload = to_json(c).dump();
    std::string to_write = payload;
    if (auto sealed = auth::crypt::seal(payload)) to_write = std::move(*sealed);
    else return false;   // refuse to write plaintext secrets

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

bool clear_codex_credentials() {
    std::scoped_lock lock{store_mutex()};
    std::error_code ec;
    return fs::remove(codex_credentials_path(), ec) || !ec;
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

std::expected<CodexCredentials, OAuthError> codex_login(int timeout_s) {
    auth::PkceVerifier verifier{auth::random_urlsafe(64)};
    auth::OAuthState   state{auth::random_urlsafe(32)};

    const std::string url = codex_authorize_url(verifier, state);
    auth::open_browser(url);

    std::atomic<bool> cancel{false};
    auto cb = wait_for_callback(timeout_s, cancel);
    if (!cb.error.empty())
        return std::unexpected(OAuthError{OAuthErrorKind::Network, cb.error});

    // Anti-CSRF: the IdP echoes our state; a mismatch means the code belongs
    // to a different login (CSRF / stale tab). Constant-time compare.
    if (!cb.state.empty()) {
        const std::string& want = state.value;
        bool ok = cb.state.size() == want.size();
        unsigned char diff = ok ? 0 : 1;
        for (std::size_t i = 0; i < want.size() && i < cb.state.size(); ++i)
            diff |= static_cast<unsigned char>(want[i] ^ cb.state[i]);
        if (!ok || diff)
            return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
                "state mismatch — re-run login"});
    }

    auto c = codex_exchange_code(auth::OAuthCode{cb.code}, verifier);
    if (!c) return c;
    if (!save_codex_credentials(*c))
        return std::unexpected(OAuthError{OAuthErrorKind::BadResponse,
            "signed in but could not save credentials"});
    return c;
}

std::optional<CodexCredentials> codex_fresh_credentials() {
    // Serialize refreshes across worker threads; first writer persists, the
    // rest re-read the fresh token.
    static std::mutex refresh_mu;
    std::scoped_lock lock{refresh_mu};

    auto loaded = load_codex_credentials();
    if (!loaded) return std::nullopt;
    if (!loaded->expired(/*skew_ms=*/60'000)) return loaded;
    if (loaded->refresh_token.empty()) return loaded;   // nothing to do; caller 401s

    auto refreshed = codex_refresh(*loaded);
    if (!refreshed) return loaded;   // network/revoked → return stale; transport surfaces 401
    save_codex_credentials(*refreshed);
    return *refreshed;
}

} // namespace agentty::provider::chatgpt
