// SPDX-License-Identifier: Apache-2.0
//
// mcp_connection_race_test.cpp — dropping an mcp::Connection while its
// reader is inside a server callback.
//
// The reader's handlers hold a strong ref to the connection's Live while they
// run. If the Connection went away then, the reader ended up with the last
// ref, and ~Peer shut down its own pool from the reader: jaal aborts. A fake
// server on an in-memory channel sends notifications/message, and the log
// callback stalls while the test destroys the Connection.
//
// Run under: -DAGENTTY_SANITIZE_ALL=thread (fine without too).

#include "agentty/mcp/connection.hpp"
#include "agentty/rpc/peer.hpp"
#include "agtest_cap.hpp"

#include <mcp/methods.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace m = ::mcp;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;
void fail(const char* what) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
}

std::atomic<bool> g_in_callback{false};
std::atomic<bool> g_dropping{false};

// The fake server: answers server/discover and tools/list, nothing else.
agentty::rpc::Dispatch server_dispatch() {
    agentty::rpc::Dispatch d;
    d.on_call = [](const maya::unlocked&, const jsonrpc::Call& c) -> jsonrpc::Maybe<std::string> {
        if (c.method == m::method::Discover) {
            m::DiscoverResult r;
            r.supportedVersions = {std::string(m::kProtocolVersion)};
            return jsonrpc::reply(c.id, m::to_json(r));
        }
        if (c.method == m::method::ListTools)
            return jsonrpc::reply(c.id, m::to_json(m::ListToolsResult{}));
        return jsonrpc::reply(c.id, std::unexpected(jsonrpc::RpcError(
            jsonrpc::errc::MethodNotFound, "not here")));
    };
    return d;
}

// One round: connect, have the server log, drop the Connection while the
// reader is still inside on_log. Before the fix this aborted.
bool drop_during_callback(int round) {
    auto [client_end, server_end] = agentty::rpc::channel_pair();
    agentty::rpc::Peer server(std::move(server_end), server_dispatch(), "fake-server");
    server.start();

    agentty::mcp::ConnectionConfig cfg;
    cfg.name = "fake";
    cfg.handshake_timeout = 5s;
    cfg.on_log = [](const m::LoggingMessageParams&) {
        g_in_callback = true;
        // Hold the reader here until the owner has started dropping the
        // Connection, then a bit longer so the drop overlaps the callback.
        while (!g_dropping) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(50ms);
    };
    agentty::mcp::Link link;
    link.open = [ch = std::move(client_end)]() {
        return agentty::mcp::Link::Opened{ch, nullptr};
    };

    g_in_callback = false;
    g_dropping = false;
    auto conn = std::make_unique<agentty::mcp::Connection>(std::move(cfg), std::move(link));

    server.notify_raw(agtest::cap(), "notifications/message",
                      jsonrpc::Json{{"level", "info"}, {"data", "round " + std::to_string(round)}});
    const auto give_up = std::chrono::steady_clock::now() + 5s;
    while (!g_in_callback && std::chrono::steady_clock::now() < give_up)
        std::this_thread::sleep_for(1ms);
    if (!g_in_callback) { fail("the log notification never reached on_log"); return false; }

    g_dropping = true;
    conn.reset();   // the reader is still inside on_log
    server.stop(agtest::cap());
    return true;
}

}  // namespace

int main() {
    std::printf("--- mcp connection: dropped while its reader runs a callback ---\n");
    for (int i = 0; i < 10; ++i)
        if (!drop_during_callback(i)) break;
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
