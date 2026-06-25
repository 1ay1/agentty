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
//   We drive the SAME mcp::RpcEngine the stdio path uses. The engine is
//   transport-agnostic: request_raw() writes a frame through our sink and
//   blocks on a promise that handle_response (fed by feed_line) fulfils. Our
//   sink POSTs the frame on a worker thread and feeds every response frame from
//   the HTTP reply back into the engine — so .get() on the engine's future
//   resolves exactly as it does over stdio.
//
//   This file lives in agentty (not mcp-cpp) because it builds on agentty's
//   own HTTP/2 client (TLS, pooling, cancellation) rather than pulling a new
//   HTTP dependency into the SDK.

#include "agentty/mcp/http_server.hpp"

#include "agentty/io/http.hpp"

#include <mcp/cap/client_provider.hpp>
#include <mcp/client.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// ── HttpTransport ─────────────────────────────────────────────────────────
// Owns the endpoint config + session id + the engine pointer. sink() POSTs
// each outbound frame on a detached worker and feeds the response back into
// the engine, so the engine's blocking .get() resolves normally.
class HttpTransport {
public:
    HttpTransport(ParsedUrl url, std::vector<http::Header> extra_headers,
                  std::chrono::milliseconds timeout)
        : url_(std::move(url)), extra_headers_(std::move(extra_headers)), timeout_(timeout) {}

    void bind(::mcp::RpcEngine* engine) { engine_ = engine; }

    void set_protocol_version(std::string v) {
        std::lock_guard<std::mutex> lk(mu_);
        protocol_version_ = std::move(v);
    }

    [[nodiscard]] bool alive() const noexcept { return alive_.load(std::memory_order_acquire); }

    void stop() {
        alive_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (cancel_) cancel_->cancel();
        }
        // Wait for any in-flight POST worker to finish before returning, so
        // the caller can safely drop the engine afterwards (no detached thread
        // touches engine_ post-stop). ClientProvider serializes calls behind
        // call_mu_, so at most one worker is ever in flight.
        std::unique_lock<std::mutex> lk(worker_mu_);
        worker_done_.wait(lk, [this] { return inflight_ == 0; });
    }

    // The Transport sink the engine writes through.
    ::mcp::Transport sink() {
        return [this](std::string_view frame) { dispatch(std::string{frame}); };
    }

private:
    // Is this outbound frame a request (needs a response) or a notification
    // (fire-and-forget, no response expected)?
    static bool is_request(const json& j) {
        return j.is_object() && j.contains("id") && j.contains("method");
    }

    void dispatch(std::string frame) {
        if (!alive_.load(std::memory_order_acquire)) return;   // stopped
        { std::lock_guard<std::mutex> lk(worker_mu_); ++inflight_; }
        // POST on a worker thread: the calling thread is the engine's
        // request_raw, which immediately blocks on the response promise. If we
        // POSTed inline we'd never feed the response (the same thread is stuck).
        std::thread([this, frame = std::move(frame)]() mutable {
            post_and_feed(std::move(frame));
            std::lock_guard<std::mutex> lk(worker_mu_);
            if (--inflight_ == 0) worker_done_.notify_all();
        }).detach();
    }

    void post_and_feed(std::string frame) {
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
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!session_id_.empty())      req.headers.push_back({"mcp-session-id", session_id_});
            if (!protocol_version_.empty()) req.headers.push_back({"mcp-protocol-version", protocol_version_});
        }
        for (const auto& h : extra_headers_) req.headers.push_back(h);

        auto cancel = std::make_shared<http::CancelToken>();
        { std::lock_guard<std::mutex> lk(mu_); cancel_ = cancel; }

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

        http::StreamHandler handler;
        handler.on_headers = [&](int status, const http::Headers& hh) {
            content_type = header_value(hh, "content-type");
            is_sse = content_type.find("text/event-stream") != std::string::npos;
            // Capture a freshly-issued session id (only on initialize, but the
            // server may send it on any response — capture whenever present).
            if (auto sid = header_value(hh, "mcp-session-id"); !sid.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                if (session_id_.empty()) session_id_ = sid;
            }
            if (status == 404 || status == 410) {
                // Session expired/unknown — drop it so the next call re-inits.
                std::lock_guard<std::mutex> lk(mu_);
                session_id_.clear();
            }
        };
        handler.on_chunk = [&](std::string_view chunk) -> bool {
            if (is_sse) { sse_buf.append(chunk); drain_sse(sse_buf); }
            else        json_buf.append(chunk);
            return true;
        };

        auto result = http::default_client().stream(req, std::move(handler), tos, cancel);

        if (!result) {
            // Transport failure. If a response was expected, synthesise a
            // JSON-RPC error so the waiting future resolves instead of hanging
            // until the engine's deadline.
            alive_.store(false, std::memory_order_release);
            if (expects_response && engine_) {
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
        } else if (expects_response && engine_) {
            // 202 Accepted with empty body for a request is a protocol error,
            // but be defensive: resolve the future with an error.
            json err = {
                {"jsonrpc", "2.0"},
                {"id", parsed.value("id", json(nullptr))},
                {"error", {{"code", -32603}, {"message", "empty HTTP response to request"}}},
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

    void feed(const std::string& payload) {
        if (!engine_) return;
        try { engine_->feed_line(payload); } catch (...) {}
    }

    ParsedUrl                 url_;
    std::vector<http::Header> extra_headers_;
    std::chrono::milliseconds timeout_;
    ::mcp::RpcEngine*         engine_ = nullptr;
    std::mutex                mu_;
    std::string               session_id_;
    std::string               protocol_version_;
    std::atomic<bool>         alive_{true};
    http::CancelTokenPtr      cancel_;
    // In-flight POST worker accounting so stop() can join before teardown.
    std::mutex                worker_mu_;
    std::condition_variable   worker_done_;
    int                       inflight_ = 0;
};

// ── HttpServerProvider ─────────────────────────────────────────────────────
class HttpServerProviderImpl final : public ::mcp::cap::ClientProvider {
public:
    HttpServerProviderImpl(const std::string& name, ParsedUrl url,
                           std::vector<http::Header> headers,
                           ::mcp::Implementation client_info,
                           std::chrono::milliseconds handshake_timeout,
                           std::chrono::milliseconds call_timeout) {
        transport_ = std::make_unique<HttpTransport>(std::move(url), std::move(headers), call_timeout);
        auto client = std::make_unique<::mcp::Client>(transport_->sink());
        transport_->bind(&client->engine());
        // After initialize the engine resolves; set the negotiated protocol
        // version header for every subsequent request. We pin the version we
        // sent (the SDK defaults to kProtocolVersion); a server that downgrades
        // is handled by the engine decoding the lower version transparently.
        transport_->set_protocol_version(std::string(::mcp::kProtocolVersion));
        connect(name, std::move(client), std::move(client_info),
                handshake_timeout, call_timeout);
    }

    ~HttpServerProviderImpl() override {
        if (transport_) transport_->stop();
        reset_client();
        transport_.reset();
    }

    [[nodiscard]] bool alive() const noexcept override {
        return transport_ && transport_->alive();
    }

protected:
    void on_teardown() noexcept override {
        if (transport_) transport_->stop();
    }

private:
    std::unique_ptr<HttpTransport> transport_;
};

} // namespace

std::shared_ptr<::mcp::cap::CapabilityProvider>
make_http_provider(const std::string& name, const HttpConfig& cfg, std::string& err) {
    ParsedUrl url = parse_url(cfg.url);
    if (!url.ok) { err = "invalid or unsupported URL: '" + cfg.url + "'"; return nullptr; }

    std::vector<http::Header> headers;
    for (const auto& [k, v] : cfg.headers) headers.push_back({k, v});

    ::mcp::Implementation client_info{"agentty", AGENTTY_VERSION};
    try {
        return std::make_shared<HttpServerProviderImpl>(
            name, std::move(url), std::move(headers), std::move(client_info),
            cfg.handshake_timeout, cfg.call_timeout);
    } catch (const std::exception& e) {
        err = std::string{"http MCP server '"} + name + "' failed: " + e.what();
    } catch (...) {
        err = std::string{"http MCP server '"} + name + "' failed (unknown)";
    }
    return nullptr;
}

} // namespace agentty::mcp
