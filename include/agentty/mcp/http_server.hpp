#pragma once
// agentty::mcp — HTTP (Streamable HTTP) MCP server provider.
//
// Builds a cap::CapabilityProvider backed by a remote MCP server reached over
// the Streamable HTTP transport (a single endpoint URL that speaks JSON-RPC
// over POST, with optional text/event-stream responses). It drives the same
// mcp::Client / RpcEngine the stdio path uses, but over agentty's own HTTP/2
// client (TLS, connection pool, cancellation) instead of a spawned process.
//
// PERF: like the stdio path, construction connects synchronously (bounded by a
// handshake timeout) and is only invoked when an `http`/`url` server is named
// in .agentty/mcp.json — a user without remote MCP servers pays nothing. The
// heavy mcp-cpp + http templates are confined to src/mcp/http_server.cpp; this
// header is dependency-light (a forward-declared cap type + a config struct).

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward-declare the cap provider type so this header doesn't drag the
// template-heavy mcp-cpp capability layer into every includer.
namespace mcp::cap { class CapabilityProvider; }

namespace agentty::mcp {

// One remote MCP server's connection config, parsed from a config entry like:
//   "deepwiki": {
//     "url": "https://mcp.deepwiki.com/mcp",
//     "headers": { "Authorization": "Bearer ..." }
//   }
struct HttpConfig {
    std::string url;        // https://host[:port]/path  (http:// allowed for localhost)
    std::vector<std::pair<std::string, std::string>> headers;   // extra request headers (auth, etc.)
    std::chrono::milliseconds handshake_timeout{15'000};
    std::chrono::milliseconds call_timeout{60'000};
};

// Connect to the server and return a CapabilityProvider, or nullptr (+ a human
// message in `err`) on any failure — never throws. The returned provider owns
// the HTTP transport + mcp::Client; keep it alive in the registry/pool.
[[nodiscard]] std::shared_ptr<::mcp::cap::CapabilityProvider>
make_http_provider(const std::string& name, const HttpConfig& cfg, std::string& err);

} // namespace agentty::mcp
