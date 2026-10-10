// agentty::mcp — HttpServerProvider: an MCP CapabilityProvider over the
// Streamable HTTP transport (MCP spec 2025-03-26+ / 2025-11-25).
//
//   The Streamable HTTP transport collapses the old HTTP+SSE pair into ONE
//   endpoint URL:
//     • client → server : HTTP POST with a single JSON-RPC message (or batch).
//       The server replies either:
//         - Content-Type: application/json  → one JSON-RPC response, or
//         - Content-Type: text/event-stream → an SSE stream whose `data:`
//           events each carry a JSON-RPC message (the response, plus any
//           server→client requests/notifications that belong to it).
//     • Mcp-Session-Id : if the server returns this header on initialize, the
//       client MUST echo it on every subsequent request. We capture + replay it.
//     • Protocol version: after initialize we send MCP-Protocol-Version on
//       every request, per the spec.
//
//   To the rest of agentty this is a byte channel, like a child's stdio: the
//   peer's writes become POSTs (on a worker, so the writer never blocks on the
//   network), and every frame in a reply (plain JSON or SSE events) is pushed
//   into an in-memory pipe the peer reads. The same mcp::Connection then runs
//   the handshake, lists and calls over it.
//
//   This file lives in agentty (not mcp-cpp) because it builds on agentty's
//   own HTTP/2 client (TLS, pooling, cancellation) rather than pulling a new
//   HTTP dependency into the SDK.

#include "agentty/mcp/http_server.hpp"
#include "agentty/mcp/oauth.hpp"

#include "agentty/io/http.hpp"
#include "agentty/util/dbglog.hpp"

#include <maya/runtime.hpp>
#include <optional>
#include <mcp/auth.hpp>
#include <mcp/protocol.hpp>

#include "agentty/mcp/connection.hpp"
#include "agentty/rpc/peer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/util/background.hpp"

#include <nlohmann/json.hpp>

namespace agentty::mcp {

namespace {

using json = nlohmann::json;

// Parse a single absolute URL into (scheme, host, port, path). Minimal — MCP
// endpoints are plain https://host[:port]/path or http://… for localhost.
struct ParsedUrl {
    bool        ok        = false;
    bool        tls       = true;
    std::string host;
    std::uint16_t port    = 443;
    std::string path      = "/";
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl u;
    std::string_view s{url};
    if (s.starts_with("https://"))      { u.tls = true;  u.port = 443; s.remove_prefix(8); }
    else if (s.starts_with("http://"))  { u.tls = false; u.port = 80;  s.remove_prefix(7); }
    else return u;   // unsupported scheme

    auto slash = s.find('/');
    std::string_view authority = slash == std::string_view::npos ? s : s.substr(0, slash);
    u.path = slash == std::string_view::npos ? std::string{"/"} : std::string{s.substr(slash)};

    auto colon = authority.find(':');
    if (colon == std::string_view::npos) {
        u.host = std::string{authority};
    } else {
        u.host = std::string{authority.substr(0, colon)};
        try {
            unsigned long port_val = std::stoul(std::string{authority.substr(colon + 1)});
            if (port_val == 0 || port_val > 65535) return u;   // out of range → !ok
            u.port = static_cast<std::uint16_t>(port_val);
        }
        catch (...) { return u; }
    }
    if (u.host.empty()) return u;
    u.ok = true;
    return u;
}

// case-insensitive header lookup over agentty's http::Headers.
std::string header_value(const http::Headers& hh, std::string_view name) {
    auto eq_ci = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = char(x + 32);
            if (y >= 'A' && y <= 'Z') y = char(y + 32);
            if (x != y) return false;
        }
        return true;
    };
    for (const auto& h : hh) if (eq_ci(h.name, name)) return h.value;
    return {};
}

// What the POST workers share with the transport: endpoint config that is
// fixed at construction, the session (guarded), the liveness flag, the cancel
// source and the feed end of the pipe. Sync, so the workers can hold it.
struct Conn {
    // Is this outbound frame a request (needs a response) or a notification
    // (fire-and-forget, no response expected)?
    static bool is_request(const json& j) {
        return j.is_object() && j.contains("id") && j.contains("method");
    }

    void fail_request(const std::string& frame, std::string_view reason) noexcept {
        alive_.store(false, std::memory_order_release);
        util::dbglog("mcp.http_transport.worker", reason);
        try {
            const auto parsed = json::parse(frame);
            if (!is_request(parsed)) return;
            json err = {
                {"jsonrpc", "2.0"},
                {"id", parsed.value("id", json(nullptr))},
                {"error", {{"code", -32003}, {"message",
                    std::string{"http transport worker: "} + std::string{reason}}}},
            };
            feed(err.dump());
        } catch (...) {
            // The transport is already marked dead; the engine deadline is the
            // final fallback if even constructing the error response fails.
        }
    }

    void post_and_feed(const std::string& frame) {
        json parsed;
        bool expects_response = false;
        try { parsed = json::parse(frame); expects_response = is_request(parsed); }
        catch (...) { /* malformed outbound — just POST it raw */ }

        http::Request req;
        req.method    = http::HttpMethod::Post;
        req.host      = url_.host;
        req.port      = url_.port;
        req.path      = url_.path;
        req.plaintext = !url_.tls;
        req.body      = frame;
        req.headers.push_back({"content-type", "application/json"});
        req.headers.push_back({"accept", "application/json, text/event-stream"});
        // MCP 2026-07-28 header-based routing (SEP-2243): surface the JSON-RPC
        // method (and, for tools/call, the tool name) as headers so an edge
        // gateway can route/authorize without parsing the body. Harmless to a
        // server that ignores them. Only meaningful on a request frame.
        if (parsed.is_object() && parsed.contains("method") && parsed["method"].is_string()) {
            const std::string m = parsed["method"].get<std::string>();
            req.headers.push_back({"mcp-method", m});
            if (m == "tools/call" && parsed.contains("params") &&
                parsed["params"].is_object() && parsed["params"].contains("name") &&
                parsed["params"]["name"].is_string()) {
                req.headers.push_back({"mcp-name", parsed["params"]["name"].get<std::string>()});
            }
        }
        {
            const auto sess = session_.read([](const Session& s) { return s; });
            if (!sess.id.empty())               req.headers.push_back({"mcp-session-id", sess.id});
            if (!sess.protocol_version.empty()) req.headers.push_back({"mcp-protocol-version", sess.protocol_version});
        }
        for (const auto& h : extra_headers_) req.headers.push_back(h);
        // MCP 2026-07-28 authorization: if the user ran `agentty mcp-login` for
        // this server, attach a fresh bearer token (auto-refreshed on expiry).
        // A statically-configured Authorization header in extra_headers_ wins
        // (we only add ours if none is present).
        if (!server_name_.empty()) {
            bool has_auth = false;
            for (const auto& h : req.headers) {
                std::string n = h.name;
                for (auto& c : n) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
                if (n == "authorization") { has_auth = true; break; }
            }
            if (!has_auth)
                if (auto bearer = oauth::bearer_for(server_name_))
                    req.headers.push_back({"authorization", *bearer});
        }

        auto cancel = cancel_;

        http::Timeouts tos;
        tos.connect = std::chrono::milliseconds(10'000);
        tos.total   = timeout_;
        tos.idle    = timeout_;

        // Stream the response so we handle both application/json (single body)
        // and text/event-stream (SSE) without buffering an unbounded stream.
        std::string content_type;
        std::string sse_buf;       // accumulates SSE bytes across chunks
        std::string json_buf;      // accumulates a non-SSE body
        bool is_sse = false;
        // This request's 401, if any. Per request: several POSTs run at once.
        bool        http_status_401_ = false;
        std::string resource_metadata_url_;

        http::StreamHandler handler;
        handler.on_headers = [&](int status, const http::Headers& hh) {
            content_type = header_value(hh, "content-type");
            is_sse = content_type.find("text/event-stream") != std::string::npos;
            // Capture a freshly-issued session id (only on initialize, but the
            // server may send it on any response — capture whenever present).
            if (auto sid = header_value(hh, "mcp-session-id"); !sid.empty()) {
                session_.with([](Session& s, std::string id) {
                    if (s.id.empty()) s.id = std::move(id);
                }, sid);
            }
            if (status == 404 || status == 410) {
                // Session expired/unknown — drop it so the next call re-inits.
                session_.with([](Session& s) { s.id.clear(); });
            }
            if (status == 401) {
                // MCP 2026-07-28 authorization (RFC 9728): the server is
                // gated behind OAuth. Parse the WWW-Authenticate challenge so
                // we can surface an actionable error instead of a bare
                // transport failure. mcp::auth::parse_challenge locates the
                // protected-resource-metadata URL.
                http_status_401_ = true;
                const std::string wa = header_value(hh, "www-authenticate");
                if (auto url = ::mcp::auth::parse_challenge(wa))
                    resource_metadata_url_ = *url;
            }
        };
        // Cap total buffered response bytes. The streaming path in
        // io/http.cpp forwards every DATA frame to on_chunk WITHOUT enforcing
        // Request::max_body_bytes (that cap only guards the unary path), so a
        // hostile or buggy MCP server could stream gigabytes — an unbounded
        // json_buf, or an SSE body that never emits a "\n\n" separator so
        // drain_sse never erases — until the process OOMs. Returning false
        // from on_chunk aborts the stream (RST) and surfaces an HttpError.
        constexpr std::size_t kMaxResponseBytes = 64ull * 1024 * 1024;
        std::size_t total_bytes = 0;
        handler.on_chunk = [&](std::string_view chunk) -> bool {
            total_bytes += chunk.size();
            if (total_bytes > kMaxResponseBytes) {
                util::dbglog("mcp.http_transport.on_chunk",
                             "response exceeded 64 MiB cap — aborting");
                return false;
            }
            if (is_sse) {
                sse_buf.append(chunk);
                drain_sse(sse_buf);
                // A single unterminated SSE event must not grow without
                // bound even while under the total cap.
                if (sse_buf.size() > kMaxResponseBytes) {
                    util::dbglog("mcp.http_transport.on_chunk",
                                 "unterminated SSE event exceeded cap");
                    return false;
                }
            } else {
                json_buf.append(chunk);
            }
            return true;
        };

        auto result = http::default_client(::agentty::IoAccess::grant()).stream(req, std::move(handler), tos, cancel);

        if (!result) {
            // Transport failure. If a response was expected, synthesise a
            // JSON-RPC error so the waiting future resolves instead of hanging
            // until the engine's deadline.
            alive_.store(false, std::memory_order_release);
            if (expects_response) {
                json err = {
                    {"jsonrpc", "2.0"},
                    {"id", parsed.value("id", json(nullptr))},
                    {"error", {{"code", -32003}, {"message",
                        std::string{"http transport: "} + result.error().render()}}},
                };
                feed(err.dump());
            }
            return;
        }

        if (is_sse) {
            drain_sse(sse_buf, /*flush=*/true);
        } else if (!json_buf.empty()) {
            // Single JSON body — may be one message or a batch array.
            feed(json_buf);
        } else if (expects_response) {
            // 202 Accepted with empty body for a request is a protocol error,
            // but be defensive: resolve the future with an error.
            std::string msg;
            if (http_status_401_) {
                // MCP 2026-07-28 authorization: the server requires OAuth. Give
                // the user something actionable rather than a generic 202.
                msg = "MCP server requires authorization (HTTP 401).";
                if (!resource_metadata_url_.empty())
                    msg += " Protected-resource metadata: " + resource_metadata_url_ +
                           ". Configure a bearer token in the server's headers, or use an"
                           " OAuth-authenticated endpoint.";
                else
                    msg += " Configure a bearer token in the server's headers.";
            } else {
                msg = "empty HTTP response to request";
            }
            json err = {
                {"jsonrpc", "2.0"},
                {"id", parsed.value("id", json(nullptr))},
                {"error", {{"code", http_status_401_ ? -32020 : -32603}, {"message", msg}}},
            };
            feed(err.dump());
        }
        // notifications with a 202/empty body: nothing to feed — correct.
    }

    // Parse complete SSE events out of `buf` and feed each event's `data:`
    // payload into the engine. An SSE event ends at a blank line ("\n\n").
    void drain_sse(std::string& buf, bool flush = false) {
        for (;;) {
            auto sep = buf.find("\n\n");
            if (sep == std::string::npos) {
                // Tolerate CRLF framing too.
                sep = buf.find("\r\n\r\n");
                if (sep == std::string::npos) break;
                feed_event(buf.substr(0, sep));
                buf.erase(0, sep + 4);
                continue;
            }
            feed_event(buf.substr(0, sep));
            buf.erase(0, sep + 2);
        }
        if (flush && !buf.empty()) { feed_event(buf); buf.clear(); }
    }

    void feed_event(const std::string& event) {
        // An SSE event is a set of `field: value` lines. We only care about
        // `data:` lines; multiple data lines concatenate with '\n'.
        std::string data;
        std::size_t pos = 0;
        while (pos < event.size()) {
            auto nl = event.find('\n', pos);
            std::string line = event.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pos = nl == std::string::npos ? event.size() : nl + 1;
            std::string_view lv{line};
            if (lv.starts_with("data:")) {
                lv.remove_prefix(5);
                if (!lv.empty() && lv.front() == ' ') lv.remove_prefix(1);
                if (!data.empty()) data += '\n';
                data.append(lv);
            }
            // ignore event:, id:, retry:, and comment (':') lines
        }
        if (!data.empty()) feed(data);
    }

    // Hand one frame (a message or a batch) to the peer. It reads lines, so
    // a pretty-printed body is compacted to one.
    void feed(const std::string& payload) {
        std::string line;
        try { line = json::parse(payload).dump(); }
        catch (...) {
            util::dbglog("mcp.http_transport.feed", "unparseable frame");
            return;
        }
        line.push_back('\n');
        (void)feed_.write(line);
    }

    const ParsedUrl                 url_;
    const std::vector<http::Header> extra_headers_;
    const std::chrono::milliseconds timeout_;
    const std::string               server_name_;   // for oauth::bearer_for lookup
    const rpc::Channel              feed_;          // our end: where we push frames
    struct Session { std::string id; std::string protocol_version; };
    maya::guarded<Session>          session_;
    std::atomic<bool>               alive_{true};
    // One token for the transport's life: stop() cancels every POST in flight.
    const http::CancelTokenPtr      cancel_ = std::make_shared<http::CancelToken>();
};

} // namespace
} // namespace agentty::mcp

namespace agentty::mcp {
namespace {

using ConnRef = maya::co_owned<Conn>;

// ── HttpTransport ─────────────────────────────────────────────────────────
// channel() is what the peer runs on: write POSTs a frame on a worker, read
// drains the frames the replies carried.
class HttpTransport {
public:
    HttpTransport(ParsedUrl url, std::vector<http::Header> extra_headers,
                  std::chrono::milliseconds timeout, std::string server_name = {}) {
        auto [peer_end, ours] = rpc::channel_pair();
        inbound_ = std::move(peer_end);
        conn_.emplace(ConnRef::make(std::move(url), std::move(extra_headers), timeout,
                                    std::move(server_name), std::move(ours)));
    }

    void set_protocol_version(std::string v) {
        (*conn_)->session_.with([](Conn::Session& s, std::string pv) { s.protocol_version = std::move(pv); },
                      std::move(v));
    }

    [[nodiscard]] bool alive() const noexcept {
        return (*conn_)->alive_.load(std::memory_order_acquire);
    }

    void stop() {
        // Admission is the WorkerGroup's job: workers_.stop() refuses further
        // posts under the pool's own lock. alive_ is the plain liveness flag
        // the provider reads.
        auto& c = **conn_;
        c.alive_.store(false, std::memory_order_release);
        c.cancel_->cancel();
        // Hard barrier: when this returns no POST worker is running.
        workers_.stop();
        if (c.feed_.interrupt) c.feed_.interrupt();   // the peer's reader sees EOF
    }

    // The channel the peer runs on. Valid while this transport lives.
    rpc::Channel channel() {
        rpc::Channel ch;
        ch.read      = inbound_.read;
        ch.write     = [this](std::string_view bytes) {
            // The peer writes whole lines; each is one JSON-RPC frame.
            std::size_t start = 0;
            while (start < bytes.size()) {
                auto nl = bytes.find('\n', start);
                auto line = bytes.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
                if (!line.empty()) dispatch(std::string{line});
                if (nl == std::string_view::npos) break;
                start = nl + 1;
            }
            return alive();
        };
        ch.interrupt = [this] { stop(); };
        return ch;
    }

private:
    void dispatch(std::string frame) {
        if (!alive()) return;   // stopped
        // POST off this thread: it is the peer's writer, and a slow server
        // must not hold up every other frame.
        workers_.post([](std::stop_token, ConnRef c, std::string frame) {
            try {
                c->post_and_feed(frame);
            } catch (const std::exception& error) {
                c->fail_request(frame, error.what());
            } catch (...) {
                c->fail_request(frame, "unknown transport exception");
            }
        }, *conn_, std::move(frame));
    }

    rpc::Channel           inbound_;   // the peer's end: frames the replies carried
    std::optional<ConnRef> conn_;      // set in the constructor, never empty after
    // POST workers. Last: stopped (joined) first.
    util::WorkerGroup      workers_{"mcp.http_transport.worker"};
};

} // namespace

std::shared_ptr<::mcp::cap::CapabilityProvider>
make_http_provider(const std::string& name, const HttpConfig& cfg, std::string& err) {
    ParsedUrl url = parse_url(cfg.url);
    if (!url.ok) { err = "invalid or unsupported URL: '" + cfg.url + "'"; return nullptr; }

    std::vector<http::Header> headers;
    for (const auto& [k, v] : cfg.headers) headers.push_back({k, v});

    ConnectionConfig cc;
    cc.name              = name;
    cc.client_info       = ::mcp::Implementation{"agentty", AGENTTY_VERSION};
    cc.handshake_timeout = cfg.handshake_timeout;
    cc.call_timeout      = cfg.call_timeout;
    if (!cfg.workspace_root.empty()) {
        std::string uri = "file://" + cfg.workspace_root;
#ifdef _WIN32
        if (cfg.workspace_root.size() > 1 && cfg.workspace_root[1] == ':') uri = "file:///" + cfg.workspace_root;
#endif
        cc.roots.push_back(::mcp::Root{std::move(uri), std::string{"workspace"}, nlohmann::json::object()});
    }
    Link link;
    link.open = [url, headers, timeout = cfg.call_timeout, name]() -> Link::Opened {
        auto t = std::make_shared<HttpTransport>(url, headers, timeout, name);
        t->set_protocol_version(std::string(::mcp::kProtocolVersion));
        return {t->channel(), t};
    };
    link.alive = [](const std::shared_ptr<void>& h) {
        return h && static_cast<HttpTransport*>(h.get())->alive();
    };
    try {
        return std::make_shared<Connection>(std::move(cc), std::move(link));
    } catch (const std::exception& e) {
        err = std::string{"http MCP server '"} + name + "' failed: " + e.what();
    } catch (...) {
        err = std::string{"http MCP server '"} + name + "' failed (unknown)";
    }
    return nullptr;
}

} // namespace agentty::mcp
