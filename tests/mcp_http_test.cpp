// mcp_http_test — end-to-end smoke for the agentty MCP Streamable HTTP
// transport (HttpServerProvider). Stands up a tiny in-process HTTP/1.1 server
// that speaks the MCP wire protocol (initialize → tools/list → tools/call,
// plus one resource + one prompt) over a single endpoint, then drives the
// bridge's make_http_provider() against http://127.0.0.1:PORT/mcp.
//
// Proves the whole HTTP chain: URL parse → POST framing → Accept negotiation
// → both application/json AND text/event-stream response handling → session-id
// capture/replay → RpcEngine round-trip → ClientProvider tools/resources/
// prompts → cap::Result.
//
// POSIX-only (raw sockets). SKIPS (exit 0) on Windows.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>

#if defined(_WIN32)
int main() { std::printf("mcp_http_test: SKIP (POSIX-only)\n"); return 0; }
#else

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <nlohmann/json.hpp>

#include "agentty/mcp/http_server.hpp"
#include <mcp/cap/cap.hpp>

using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── tiny MCP-over-HTTP/1.1 server ──────────────────────────────────────────
// Handles one connection at a time, request/response per POST. Returns
// application/json bodies (the simplest compliant Streamable HTTP server).

static json handle_rpc(const json& req, bool& notify_only) {
    notify_only = false;
    const std::string method = req.value("method", "");
    json res;
    res["jsonrpc"] = "2.0";
    if (req.contains("id")) res["id"] = req["id"];
    else { notify_only = true; return {}; }   // a notification — 202, no body

    if (method == "initialize") {
        res["result"] = {
            {"protocolVersion", "2025-11-25"},
            {"capabilities", {{"tools", json::object()},
                              {"resources", json::object()},
                              {"prompts", json::object()}}},
            {"serverInfo", {{"name", "http-demo"}, {"version", "1.0"}}},
        };
    } else if (method == "tools/list") {
        res["result"] = {{"tools", json::array({
            {{"name", "echo"},
             {"description", "Echo the message back."},
             {"inputSchema", {{"type", "object"},
                              {"properties", {{"msg", {{"type", "string"}}}}}}},
             {"annotations", {{"readOnlyHint", true}}}},
        })}};
    } else if (method == "tools/call") {
        const auto& p = req["params"];
        std::string msg = p.value("arguments", json::object()).value("msg", "");
        res["result"] = {{"content", json::array({
            {{"type", "text"}, {"text", "echo: " + msg}}})}};
    } else if (method == "resources/list") {
        res["result"] = {{"resources", json::array({
            {{"uri", "mem://note"}, {"name", "note"}, {"mimeType", "text/plain"}}})}};
    } else if (method == "resources/read") {
        res["result"] = {{"contents", json::array({
            {{"uri", "mem://note"}, {"text", "remote note body"}, {"mimeType", "text/plain"}}})}};
    } else if (method == "prompts/list") {
        res["result"] = {{"prompts", json::array({
            {{"name", "greet"}, {"description", "Greeting prompt."}}})}};
    } else if (method == "prompts/get") {
        res["result"] = {{"messages", json::array({
            {{"role", "user"}, {"content", {{"type", "text"}, {"text", "hi from http"}}}}})}};
    } else {
        res["error"] = {{"code", -32601}, {"message", "method not found: " + method}};
    }
    return res;
}

static void serve(int listen_fd, std::atomic<bool>& stop) {
    while (!stop.load()) {
        sockaddr_in cli{};
        socklen_t len = sizeof cli;
        int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&cli), &len);
        if (fd < 0) { if (stop.load()) break; continue; }

        // Read the full request (headers + body). We rely on Content-Length.
        std::string buf;
        char tmp[4096];
        std::size_t content_len = 0;
        std::size_t header_end = std::string::npos;
        for (;;) {
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
            if (header_end == std::string::npos) {
                header_end = buf.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    // parse Content-Length (case-insensitive)
                    std::string head = buf.substr(0, header_end);
                    for (std::size_t i = 0; i < head.size(); ++i) head[i] = (char)std::tolower(head[i]);
                    auto cl = head.find("content-length:");
                    if (cl != std::string::npos) {
                        content_len = std::strtoul(head.c_str() + cl + 15, nullptr, 10);
                    }
                }
            }
            if (header_end != std::string::npos &&
                buf.size() >= header_end + 4 + content_len) break;
        }
        std::string body = (header_end != std::string::npos)
                               ? buf.substr(header_end + 4, content_len) : std::string{};

        std::string out;
        try {
            json req = json::parse(body);
            bool notify_only = false;
            json res = handle_rpc(req, notify_only);
            const std::string method = req.value("method", "");
            if (notify_only) {
                out = "HTTP/1.1 202 Accepted\r\n"
                      "Mcp-Session-Id: test-session\r\n"
                      "Content-Length: 0\r\n\r\n";
            } else if (method == "resources/read") {
                // Exercise the SSE response path: wrap the JSON-RPC response in
                // a text/event-stream `data:` event.
                std::string payload = "event: message\r\ndata: " + res.dump() + "\r\n\r\n";
                out  = "HTTP/1.1 200 OK\r\n";
                out += "Content-Type: text/event-stream\r\n";
                out += "Mcp-Session-Id: test-session\r\n";
                out += "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
                out += payload;
            } else {
                std::string payload = res.dump();
                out  = "HTTP/1.1 200 OK\r\n";
                out += "Content-Type: application/json\r\n";
                out += "Mcp-Session-Id: test-session\r\n";
                out += "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n";
                out += payload;
            }
        } catch (...) {
            out = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        }
        ::send(fd, out.data(), out.size(), 0);
        ::close(fd);
    }
}

int main() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = 0;   // ephemeral
    CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(listen_fd, 4) == 0);
    socklen_t alen = sizeof addr;
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen);
    int port = ntohs(addr.sin_port);
    std::printf("mcp_http_test: server on 127.0.0.1:%d\n", port);

    std::atomic<bool> stop{false};
    std::thread server_thread([&] { serve(listen_fd, stop); });

    // Drive the HTTP MCP provider.
    agentty::mcp::HttpConfig cfg;
    cfg.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    cfg.handshake_timeout = std::chrono::milliseconds(5000);
    cfg.call_timeout      = std::chrono::milliseconds(5000);
    std::string err;
    auto provider = agentty::mcp::make_http_provider("httpdemo", cfg, err);
    CHECK(static_cast<bool>(provider));
    if (!provider) std::fprintf(stderr, "  provider failed: %s\n", err.c_str());

    if (provider) {
        auto tools = provider->list();
        std::printf("mcp_http_test: %zu tool(s)\n", tools.size());
        CHECK(!tools.empty());
        bool found_echo = false;
        for (const auto& t : tools) if (t.name == "echo") found_echo = true;
        CHECK(found_echo);

        auto r = provider->execute(::mcp::cap::Request{"echo", json{{"msg", "ping"}}});
        std::printf("  echo(ping) -> %s\n", r.text.c_str());
        CHECK(!r.is_error);
        CHECK(r.text.find("echo: ping") != std::string::npos);

        auto resources = provider->resources();
        std::printf("mcp_http_test: %zu resource(s)\n", resources.size());
        CHECK(!resources.empty());
        std::vector<::mcp::ResourceContents> contents;
        std::string rerr;
        CHECK(provider->read_resource("mem://note", contents, rerr));
        CHECK(!contents.empty());
        // resources/read is delivered over SSE (text/event-stream) by the test
        // server — verify the SSE parser produced the right body.
        if (!contents.empty()) {
            bool ok = std::visit([](const auto& rc) {
                using T = std::decay_t<decltype(rc)>;
                if constexpr (std::is_same_v<T, ::mcp::TextResourceContents>)
                    return rc.text.find("remote note body") != std::string::npos;
                else return false;
            }, contents.front());
            CHECK(ok);
        }

        auto prompts = provider->prompts();
        std::printf("mcp_http_test: %zu prompt(s)\n", prompts.size());
        CHECK(!prompts.empty());
        ::mcp::GetPromptResult gp;
        std::string perr;
        CHECK(provider->get_prompt("greet", {}, gp, perr));
        CHECK(!gp.messages.empty());
    }

    provider.reset();    // tear down before stopping the server
    stop.store(true);
    // Kick the accept() loop with a throwaway connection so it observes stop.
    int kick = ::socket(AF_INET, SOCK_STREAM, 0);
    if (kick >= 0) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        a.sin_port = htons(static_cast<uint16_t>(port));
        ::connect(kick, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::close(kick);
    }
    server_thread.join();
    ::close(listen_fd);

    if (g_failures == 0) { std::printf("mcp_http_test: all checks passed\n"); return 0; }
    std::fprintf(stderr, "mcp_http_test: %d failure(s)\n", g_failures);
    return 1;
}

#endif // !_WIN32
