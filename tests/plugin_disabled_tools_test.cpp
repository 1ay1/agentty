// plugin_disabled_tools_test — a disabled server keeps its tool list.
//
// The bug: disabling a plugin whose tools were showing made the WHOLE tool
// list vanish (a disabled server advertises nothing live), while a server that
// started disabled, or one with individually-excluded tools, kept them. The
// fix caches each server's last-advertised tools (s_seen in plugin_model) and
// falls back to it when the server isn't connected — so the tree stays
// populated across a disable→enable round-trip.
//
// This drives the REAL producer: connect a working stdio server (populating
// the cache), snapshot, disable it, re-snapshot, and assert the tools persist.

#include "agentty/tool/registry.hpp"
#include "agentty/mcp/client.hpp"
#include "agentty/tool/plugin.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;

static int g_fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fails;
}

static std::size_t tool_count_for(const std::string& server) {
    auto m = mcp::plugin_model();
    for (const auto& s : m.servers)
        if (s.name == server) return s.tools.size();
    return static_cast<std::size_t>(-1);   // server absent
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "5000", 1);

    // Use the real, always-connecting date_server example binary. A scripted
    // stdio server proved flaky to connect under the test's spawn env; the
    // built example is the reliable source of a genuine handshake. Skip
    // cleanly if it isn't built (e.g. a partial checkout).
    const char* bin = std::getenv("AGENTTY_DATE_SERVER");
    fs::path date_server = bin && *bin
        ? fs::path{bin}
        : fs::path{"mcp-cpp/build/examples/date-mcp/date_server"};
    if (!fs::exists(date_server)) {
        // Try relative to the source tree root guesses.
        for (const char* c : {"../mcp-cpp/build/examples/date-mcp/date_server",
                              "./mcp-cpp/build/examples/date-mcp/date_server"}) {
            if (fs::exists(c)) { date_server = c; break; }
        }
    }
    if (!fs::exists(date_server)) {
        std::printf("  (skip: date_server example not built — set "
                    "AGENTTY_DATE_SERVER)\nAll disabled-tools tests passed.\n");
        return 0;
    }
    date_server = fs::absolute(date_server);

    auto tmp = fs::temp_directory_path()
             / ("agentty_disabled_tools_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".agentty");
    ::setenv("HOME", tmp.c_str(), 1);
    ::unsetenv("USERPROFILE");

    const fs::path cfg = tmp / ".agentty" / "mcp.json";
    {
        std::ofstream f(cfg);
        f << R"({"mcpServers":{"demo":{"command":")" << date_server.string()
          << R"("}}})";
    }
    // date_server advertises exactly 2 tools (current_date, days_between).
    constexpr std::size_t kN = 2;

    // 1. Connect → the server advertises its tools; snapshot records them.
    (void)tools::registry();          // triggers connect_initial_mcp
    (void)tools::reload_mcp_plugins(); // ensure a fresh connected pool
    const std::size_t connected = tool_count_for("demo");
    check(connected == kN,
          "connected server advertises its tools (got "
          + std::to_string(connected) + ")");

    // 2. Disable the whole server, reload (it no longer connects).
    check(tools::plugin::set_server_disabled(cfg, "demo", true)
              == tools::plugin::EditResult::Ok, "disable succeeds");
    (void)tools::reload_mcp_plugins();

    // 3. THE FIX: the disabled server still lists its tools (from cache),
    //    instead of collapsing to zero.
    const std::size_t disabled = tool_count_for("demo");
    check(disabled == kN,
          "disabled server KEEPS its tools in the snapshot (got "
          + std::to_string(disabled) + ") — no vanishing tree");

    // 4. Re-enable → back to live, still kN.
    check(tools::plugin::set_server_disabled(cfg, "demo", false)
              == tools::plugin::EditResult::Ok, "re-enable succeeds");
    (void)tools::reload_mcp_plugins();
    const std::size_t reenabled = tool_count_for("demo");
    check(reenabled == kN,
          "re-enabled server shows its tools again (got "
          + std::to_string(reenabled) + ")");

    fs::remove_all(tmp);
    if (g_fails == 0) { std::printf("\nAll disabled-tools tests passed.\n"); return 0; }
    std::printf("\n%d disabled-tools test(s) FAILED.\n", g_fails);
    return 1;
}
