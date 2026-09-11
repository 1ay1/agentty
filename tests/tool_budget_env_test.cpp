// tool_budget_env_test — the MCP tool budget is configurable via
// $AGENTTY_MCP_TOOL_BUDGET.
//
// Previously the cap was the compile-time constant kToolBudget = 100 with no
// way to change it. Now plugin_model() and the wire projection read the env
// var live (positive = cap, 0 = no cap, unparsable/negative = default 100),
// matching the AGENTTY_MCP_TIMEOUT_MS pattern in the same file.
//
// Drives the REAL producer: a connected stdio server advertising 2 tools
// against a configured budget smaller than native + 2 — asserting on BOTH
// the plugin_model() view (over_budget flags, trimmed_count, tool_budget)
// AND the actual wire catalog (wire_tools_snapshot()), so a silent drift
// between "what the picker warns about" and "what ships" cannot come back.

#include "agentty/tool/registry.hpp"
#include "agentty/mcp/client.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace agentty;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fails;
}

// Tools of the demo server currently on the wire (bare count + flagged view).
// ToolDef::name is the strong Id<ToolNameTag> — compare via its .value.
static std::size_t demo_tools_on_wire() {
    std::size_t n = 0;
    for (const auto& t : tools::wire_tools_snapshot())
        if (t.name.value.rfind("mcp__demo__", 0) == 0) ++n;
    return n;
}

}  // namespace (fold)

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    ::setenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS", "5000", 1);
    ::unsetenv("AGENTTY_MCP_TOOL_BUDGET");   // section 1 asserts the default

    // Same reliable fixture as plugin_disabled_tools_test: the real
    // always-connecting date_server example (a scripted stdio server proved
    // flaky under the test's spawn env). Skip cleanly if it isn't built.
    const char* bin = std::getenv("AGENTTY_DATE_SERVER");
    fs::path date_server = bin && *bin
        ? fs::path{bin}
        : fs::path{"mcp-cpp/build/examples/date-mcp/date_server"};
    if (!fs::exists(date_server)) {
        for (const char* c : {"../mcp-cpp/build/examples/date-mcp/date_server",
                              "./mcp-cpp/build/examples/date-mcp/date_server"}) {
            if (fs::exists(c)) { date_server = c; break; }
        }
    }
    if (!fs::exists(date_server)) {
        std::printf("  (skip: date_server example not built — set "
                    "AGENTTY_DATE_SERVER)\nAll tool-budget tests passed.\n");
        return 0;
    }
    date_server = fs::absolute(date_server);

    auto tmp = fs::temp_directory_path()
             / ("agentty_tool_budget_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".agentty");
    // Isolate via AGENTTY_HOME (user_root() resolves it FIRST) AND $HOME —
    // the folded standalone main pins AGENTTY_HOME for the whole binary.
    ::setenv("AGENTTY_HOME", (tmp / ".agentty").c_str(), 1);
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

    (void)tools::registry();            // triggers connect_initial_mcp
    (void)tools::reload_mcp_plugins();  // fresh connected pool

    // The native count is machine/config-dependent — derive the budget from
    // it so the assertions hold wherever this runs. The registry is the same
    // source plugin_model() uses for its budget math.
    const std::size_t native = tools::native_registry().size();
    check(native > 0, "native registry is non-empty");
    check(demo_tools_on_wire() == kN,
          "sanity: both demo tools are on the wire at the default budget");

    // ── 1. Default: no env var → budget 100, nothing trimmed ──────────────
    {
        auto m = mcp::plugin_model();
        check(m.tool_budget == 100,
              "default budget is 100 without AGENTTY_MCP_TOOL_BUDGET (got "
              + std::to_string(m.tool_budget) + ")");
        check(!m.over_budget() && m.trimmed() == 0,
              "default budget trims nothing");
    }

    // ── 2. Budget = native + 1 → exactly one demo tool ships, one is
    //       trimmed from the wire AND flagged in the model ─────────────────
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", std::to_string(native + 1).c_str(), 1);
        (void)tools::reload_mcp_plugins();   // re-projects the wire catalog
        auto m = mcp::plugin_model();
        check(m.tool_budget == native + 1,
              "env var sets the budget (native+1="
              + std::to_string(native + 1) + ")");
        // wire_tool_count is deliberately the PRE-trim total (native + every
        // enabled MCP tool) — the picker renders "N tools (budget B); M over
        // budget were dropped" from it, so it must NOT subtract the trims.
        check(m.wire_tool_count == native + kN,
              "wire_tool_count reports the pre-trim total (got "
              + std::to_string(m.wire_tool_count) + ", want "
              + std::to_string(native + kN) + ")");
        check(m.trimmed() == kN - 1,
              "model counts the trimmed tool (got " + std::to_string(m.trimmed())
              + ", want " + std::to_string(kN - 1) + ")");

        // Model view: one demo tool flagged over_budget, one not.
        std::size_t flagged = 0, clean = 0;
        for (const auto& s : m.servers)
            if (s.name == "demo")
                for (const auto& t : s.tools)
                    (t.over_budget ? flagged : clean) += 1;
        check(flagged == kN - 1 && clean == 1,
              "exactly the past-budget demo tools are flagged over_budget "
              "(flagged=" + std::to_string(flagged) + " clean="
              + std::to_string(clean) + ")");

        // Wire view: the projection REALLY trimmed the MCP tail — native
        // tools always ship, only one demo tool remains.
        const std::size_t on_wire = demo_tools_on_wire();
        check(on_wire == 1,
              "wire projection trims MCP tools past the budget (demo tools "
              "on wire=" + std::to_string(on_wire) + ", want 1)");
        check(tools::wire_tools_snapshot().size() == native + on_wire,
              "wire catalog stays native + room worth of MCP tools");
    }

    // ── 3. Budget 0 disables the cap — every advertised tool ships ────────
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", "0", 1);
        (void)tools::reload_mcp_plugins();
        auto m = mcp::plugin_model();
        check(m.tool_budget == 0, "budget 0 reads through as 'unset cap'");
        check(!m.over_budget() && m.trimmed() == 0,
              "budget 0 flags nothing over budget");
        check(demo_tools_on_wire() == kN,
              "budget 0 puts every demo tool back on the wire");
    }

    // ── 4. Unparsable value falls back to the default ─────────────────────
    {
        ::setenv("AGENTTY_MCP_TOOL_BUDGET", "not-a-number", 1);
        (void)tools::reload_mcp_plugins();
        auto m = mcp::plugin_model();
        check(m.tool_budget == 100,
              "unparsable budget falls back to the default 100 (got "
              + std::to_string(m.tool_budget) + ")");
    }

    // ── 5. Cleanup: leave the process env as we found it ──────────────────
    ::unsetenv("AGENTTY_MCP_TOOL_BUDGET");
    (void)tools::reload_mcp_plugins();

    fs::remove_all(tmp);
    if (g_fails == 0) { std::printf("\nAll tool-budget tests passed.\n"); return 0; }
    std::printf("\n%d tool-budget test(s) FAILED.\n", g_fails);
    return 1;
}
