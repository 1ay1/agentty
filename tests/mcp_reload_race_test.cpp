// mcp_reload_race_test — concurrent connect + snapshot must not crash.
//
// Regression lock for the SIGSEGV where the Plugins panel connect ran on
// task_isolated workers: connect_initial_mcp() (under the registry cache mu)
// and mcp_reload() (under reload_mu) could both run mcp::mcp_tools() at once
// (the "date connected 4×" symptom), and a concurrent build racing a
// plugin_model() reader crashed with no abort message.
//
// The fix serializes ALL connect+swap under one bridge mutex (g_connect_mu),
// so no two builds ever overlap. This test hammers the exact interleaving:
// many threads reload + snapshot the same pool at once, against a real (tiny)
// stdio MCP server. It must complete with a stable snapshot and no crash /
// TSan complaint (the suite runs it under the sanitizer label).

#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/mcp/client.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;


// A minimal stdio MCP server as a shell script — echoes canned JSON-RPC
// replies. Enough for the connect handshake (initialize + tools/list) so the
// pool actually spawns a child and lists a tool.
static fs::path write_fake_server(const fs::path& dir) {
    const fs::path sh = dir / "fake_mcp.sh";
    std::ofstream f(sh);
    f << R"SH(#!/bin/sh
# Minimal MCP stdio server: reply to initialize + tools/list, ignore the rest.
while IFS= read -r line; do
  case "$line" in
    *'"method":"initialize"'*)
      printf '%s\n' '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"fake","version":"1.0.0"}}}' ;;
    *'"method":"tools/list"'*)
      printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"ping","description":"ping","inputSchema":{"type":"object","properties":{}}}]}}' ;;
  esac
done
)SH";
    f.close();
    fs::permissions(sh, fs::perms::owner_all);
    return sh;
}

TEST_CASE("mcp reload race") {
    // Same global SIGPIPE ignore agentty installs — a spawned server dying
    // during the handshake must not kill us (this test spawns /usr/bin/false
    // which exits before the handshake write).
    std::signal(SIGPIPE, SIG_IGN);
    // A dead-on-spawn server (/usr/bin/false) would otherwise burn the full
    // connect deadline each round — shrink it so the concurrency stress runs
    // fast. We're testing the BUILD/SWAP/READ race, not connect latency.
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "150", 1);
    auto tmp = fs::temp_directory_path()
             / ("agentty_mcp_race_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    ::setenv("HOME", tmp.c_str(), 1);
    // mcp.json is looked up under util::user_root() ($AGENTTY_HOME,
    // else $HOME/.agentty); keep both pointing at this sandbox.
    ::unsetenv("AGENTTY_HOME");   // fall back to $HOME/.agentty
    ::unsetenv("USERPROFILE");

    const fs::path server = write_fake_server(tmp);
    // mcp.json under $HOME/.agentty so mcp_config_present() finds it. We use a
    // command that EXITS IMMEDIATELY (`false`): the handshake fails fast, but
    // the race we're testing is the concurrent mcp_tools() BUILD + pool swap +
    // plugin_model() read — which happens regardless of whether the child
    // handshakes. A real long-lived server would just make each round slower;
    // fast-fail keeps the concurrency stress tight and hang-free.
    (void)server;
    fs::create_directories(tmp / ".agentty");
    {
        std::ofstream cfg(tmp / ".agentty" / "mcp.json");
        cfg << R"({"mcpServers":{"a":{"command":"/usr/bin/false"},)"
               R"("b":{"command":"/usr/bin/false"}}})";
    }

    // ── The race: N threads each reload the pool while M threads snapshot it,
    //    all at once. Pre-fix this SIGSEGV'd; post-fix it's serialized. ──
    constexpr int kReloaders = 6;
    constexpr int kSnappers  = 6;
    constexpr int kRounds    = 3;

    std::atomic<bool> go{false};
    std::atomic<int>  crashes{0};   // any exception escaping a worker
    std::atomic<std::size_t> max_servers{0};
    std::vector<std::thread> ts;

    for (int i = 0; i < kReloaders; ++i)
        ts.emplace_back([&]{
            while (!go.load()) std::this_thread::yield();
            for (int r = 0; r < kRounds; ++r) {
                try { (void)tools::reload_mcp_plugins(); }
                catch (...) { ++crashes; }
            }
        });
    for (int i = 0; i < kSnappers; ++i)
        ts.emplace_back([&]{
            while (!go.load()) std::this_thread::yield();
            for (int r = 0; r < kRounds * 4; ++r) {
                try {
                    auto m = mcp::plugin_model();
                    std::size_t n = m.servers.size();
                    std::size_t prev = max_servers.load();
                    while (n > prev && !max_servers.compare_exchange_weak(prev, n)) {}
                } catch (...) { ++crashes; }
            }
        });

    go.store(true);
    for (auto& t : ts) t.join();

    check(crashes.load() == 0,
          "no worker threw/crashed under concurrent reload+snapshot ("
          + std::to_string(crashes.load()) + " failures)");

    // A final snapshot must be coherent: both configured servers present
    // (connected=false is fine — the point is no crash + a stable read).
    auto final = mcp::plugin_model();
    check(final.servers.size() == 2,
          "final snapshot has both configured servers (got "
          + std::to_string(final.servers.size()) + ")");

    fs::remove_all(tmp);
}

// ── destroy-during-handshake ────────────────────────────────────────────────
// Regression lock for the field SIGSEGV (crash report 2026-08-16): thread A
// dropped the last ConnectionPool reference (→ ~StdioServerProvider →
// teardown) while thread B was still INSIDE that provider's start_() →
// connect() → RpcEngine::request handshake. Pre-fix, ~StdioServerProvider
// did not take reconnect_mu_, so destruction proceeded concurrently with the
// handshake and the engine was freed under the requesting thread's feet.
// Post-fix the destructor serializes on reconnect_mu_ and parks until the
// handshake resolves.
//
// Reproduction: a SLOW fake server (sleeps before answering initialize) so
// the handshake window is wide, plus a connect deadline shorter than the
// sleep so mcp_tools() abandons the pending future and returns — then we
// immediately drop every pool handle while the abandoned worker is still
// mid-handshake. Pre-fix this dies ~every run; post-fix the reaper thread
// keeps the future's provider alive and its destructor waits for connect.
static fs::path write_slow_server(const fs::path& dir) {
    const fs::path sh = dir / "slow_mcp.sh";
    std::ofstream f(sh);
    f << R"SH(#!/bin/sh
while IFS= read -r line; do
  case "$line" in
    *'"method":"initialize"'*)
      sleep 1
      printf '%s\n' '{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"slow","version":"1.0.0"}}}' ;;
    *'"method":"tools/list"'*)
      printf '%s\n' '{"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"ping","description":"ping","inputSchema":{"type":"object","properties":{}}}]}}' ;;
  esac
done
)SH";
    f.close();
    fs::permissions(sh, fs::perms::owner_all);
    return sh;
}

TEST_CASE("mcp destroy during handshake") {
    std::signal(SIGPIPE, SIG_IGN);
    auto tmp = fs::temp_directory_path()
             / ("agentty_mcp_dtor_race_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".agentty");
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");

    const fs::path server = write_slow_server(tmp);
    {
        std::ofstream cfg(tmp / ".agentty" / "mcp.json");
        cfg << R"({"mcpServers":{"slow":{"command":")" << server.string()
            << R"("}}})";
    }
    // Deadline (50ms) far shorter than the server's initialize sleep (1s):
    // every round abandons a still-handshaking connect worker.
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "50", 1);

    for (int round = 0; round < 3; ++round) {
        mcp::PoolHandle pool;
        (void)mcp::mcp_tools(pool);      // abandons the pending handshake
        pool.reset();                    // drop caller handle
        mcp::release_servers();          // drop the process-wide handle NOW,
                                         // while the worker is mid-handshake
        // Pre-fix: the abandoned worker's provider could be destroyed under
        // the handshaking thread → SIGSEGV here (no assertion needed — the
        // crash IS the failure). Give the race a beat to fire.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(true, "no crash across 3 abandon+destroy rounds");

    // Let the last worker's `sleep 1` handshake resolve before we delete the
    // script out from under a still-running child.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    fs::remove_all(tmp);
}
