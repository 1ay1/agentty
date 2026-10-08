#pragma once
// agentty::mcp::Connection — one MCP server, as a cap::CapabilityProvider.
//
// mcp-cpp has no client connection of its own: it declares the methods and
// answers the server's callbacks (mcp::ClientHandlers). This is the part that
// runs: an rpc::Peer over a byte channel (a child's stdio, or an HTTP
// transport presented as one), the initialize/discover handshake, the cached
// tools/resources/prompts lists, and tool calls with MRTR and cancel.
//
// A Link opens the channel. reconnect() asks it for a fresh one, so the
// stdio and HTTP flavours differ only in their Link.

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <maya/runtime.hpp>
#include <mcp/cap/capability.hpp>
#include <mcp/cap/process.hpp>
#include <mcp/client.hpp>

#include "agentty/rpc/peer.hpp"

namespace agentty::mcp {

// How a Connection reaches its server.
struct Link {
    // Open a fresh channel (spawn the child, set up the HTTP session). Throws
    // on failure. The returned holder keeps whatever backs the channel alive
    // and is dropped after the peer stops.
    struct Opened {
        rpc::Channel          channel;
        std::shared_ptr<void> holder;
    };
    std::function<Opened()> open;
    // Is what backs the channel still up (child alive, HTTP not failed)?
    std::function<bool(const std::shared_ptr<void>& holder)> alive;
};

// A server spawned as a child process, spoken to over its stdio.
[[nodiscard]] Link stdio_link(::mcp::cap::ChildProcess::Spawn spawn);

struct ConnectionConfig {
    std::string                 name;   // origin becomes "mcp:<name>"
    ::mcp::Implementation       client_info{"agentty", "0"};
    std::chrono::milliseconds   handshake_timeout{10'000};
    std::chrono::milliseconds   call_timeout{60'000};
    std::vector<::mcp::Root>    roots;
    std::function<void(const ::mcp::ResourceUpdatedParams&)> on_resource_updated;
    std::function<void(const ::mcp::LoggingMessageParams&)>  on_log;
};

class Connection final : public ::mcp::cap::CapabilityProvider {
public:
    // Connects now; throws if the handshake fails.
    Connection(ConnectionConfig cfg, Link link);
    ~Connection() override;

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] std::string_view origin() const noexcept override { return origin_; }

    [[nodiscard]] std::vector<::mcp::Tool> list() const override;
    [[nodiscard]] ::mcp::cap::Result execute(const ::mcp::cap::Request& req) override;

    [[nodiscard]] std::vector<::mcp::Resource> resources() const override;
    [[nodiscard]] std::vector<::mcp::ResourceTemplate> resource_templates() const override;
    [[nodiscard]] bool read_resource(const std::string& uri,
                                     std::vector<::mcp::ResourceContents>& out,
                                     std::string& err) override;

    [[nodiscard]] std::vector<::mcp::Prompt> prompts() const override;
    [[nodiscard]] bool get_prompt(const std::string& name,
                                  const std::vector<std::pair<std::string, std::string>>& args,
                                  ::mcp::GetPromptResult& out,
                                  std::string& err) override;

    void set_on_list_changed(ListChangedFn fn) override;

    [[nodiscard]] bool alive() const;
    [[nodiscard]] const ::mcp::ServerCapabilities& server_capabilities() const noexcept { return caps_; }

private:
    struct Live;   // the peer, the holder, the cached lists

    // Make sure a healthy connection is up, reconnecting if needed. Returns
    // the live state, or an error.
    std::expected<std::shared_ptr<Live>, std::string> ensure_live();
    std::shared_ptr<Live> connect();
    ::mcp::ClientHandlers handlers(std::weak_ptr<Live> live);

    void refresh_tools(Live& l) const;
    void refresh_resources(Live& l) const;
    void refresh_prompts(Live& l) const;

    ConnectionConfig          cfg_;
    Link                      link_;
    std::string               origin_;
    ::mcp::ServerCapabilities caps_{};

    // The live connection, swapped whole on reconnect. Readers take a
    // snapshot; a call keeps its snapshot alive for its duration.
    maya::published<Live> live_;
    // One reconnect at a time.
    maya::guarded<bool>   reconnecting_;
    // The host's list_changed callback, read from the peer's reader.
    maya::published<const ListChangedFn> on_changed_;
};

}  // namespace agentty::mcp
