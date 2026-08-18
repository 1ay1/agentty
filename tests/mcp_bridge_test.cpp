// mcp_bridge_test — end-to-end smoke for the agentty↔mcp-cpp capability
// bridge. Spawns the mcp-cpp example MCP server (built into the submodule)
// via a generated mcp.json, then drives agentty::mcp::mcp_tools() — proving
// the whole chain: config parse → cap::StdioServerProvider spawn+handshake →
// cap::Registry → synthesized ToolDef → execute() round-trips a tools/call.
//
// SKIPS (exit 0) when the example server binary isn't built, so the suite
// stays green on machines that didn't build mcp-cpp examples. Set
// AGENTTY_MCP_E2E_SERVER to point at a server binary explicitly.

#include "agentty/mcp/client.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>   // getpid
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Locate the example MCP server: env override, then a few build-tree guesses
// relative to common CWDs (repo root or build/).
static std::string find_server() {
    if (const char* e = std::getenv("AGENTTY_MCP_E2E_SERVER"); e && e[0]) {
        if (fs::exists(e)) return e;
    }
    const char* candidates[] = {
        "mcp-cpp/build/examples/mcp_server_example",
        "../mcp-cpp/build/examples/mcp_server_example",
        "mcp-cpp/build/examples/Release/mcp_server_example",
    };
    std::error_code ec;
    for (const char* c : candidates)
        if (fs::is_regular_file(c, ec)) return fs::absolute(c, ec).string();
    return {};
}

}  // namespace (fold)

int main() {
    // ── Robustness: a server whose command does NOT exist must be SKIPPED
    // cleanly (no spawn, no hang, no crash), returning zero tools. This is
    // the exact failure a wrong path in mcp.json produces — it must never
    // take down the session. Runs unconditionally (no example server
    // needed): a bogus absolute path can't resolve on any machine.
    {
        auto btmp = fs::temp_directory_path() /
                    ("agentty_mcp_badpath_" + std::to_string(::getpid()));
        std::error_code bec; fs::remove_all(btmp, bec);
        fs::create_directories(btmp, bec);
        auto bcfg = btmp / "mcp.json";
        {
            std::ofstream f(bcfg);
            f << "{ \"mcpServers\": { \"ghost\": { \"command\": "
              << "\"/nonexistent/definitely/not/here/date_server\" } } }\n";
        }
        ::setenv("AGENTTY_MCP_CONFIG", bcfg.string().c_str(), 1);
        CHECK(mcp::mcp_config_present());
        mcp::PoolHandle bpool;
        auto btools = mcp::mcp_tools(bpool);
        std::printf("mcp_bridge_test: bad-path config → %zu tool(s) "
                    "(expect 0)\n", btools.size());
        CHECK(btools.empty());   // skipped, not spawned; session survives
        ::unsetenv("AGENTTY_MCP_CONFIG");
        fs::remove_all(btmp, bec);
    }

    std::string server = find_server();
    if (server.empty()) {
        std::printf("mcp_bridge_test: SKIP (no example server built; "
                    "build mcp-cpp with -DMCP_BUILD_EXAMPLES=ON or set "
                    "AGENTTY_MCP_E2E_SERVER)\n");
        return 0;
    }

    // Write a temp mcp.json pointing at the example server, and aim the bridge
    // at it via AGENTTY_MCP_CONFIG.
    // PID-unique so the suite stays hermetic under parallel CTest (-j).
    auto tmp = fs::temp_directory_path() /
               ("agentty_mcp_e2e_" + std::to_string(::getpid()));
    std::error_code ec; fs::remove_all(tmp, ec); fs::create_directories(tmp, ec);
    auto cfg = tmp / "mcp.json";
    {
        std::ofstream f(cfg);
        f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
          << server << "\" } } }\n";
    }
    ::setenv("AGENTTY_MCP_CONFIG", cfg.string().c_str(), 1);

    CHECK(mcp::mcp_config_present());

    mcp::PoolHandle pool;
    auto tools = mcp::mcp_tools(pool);
    std::printf("mcp_bridge_test: bridge returned %zu tool(s)\n", tools.size());
    for (const auto& t : tools)
        std::printf("  - %s\n", t.name.value.c_str());

    // The example server advertises at least "add" and "now".
    CHECK(!tools.empty());
    CHECK(static_cast<bool>(pool));   // keep-alive handle populated

    const tools::ToolDef* add = nullptr;
    for (const auto& t : tools)
        if (t.name.value.find("add") != std::string::npos) add = &t;
    CHECK(add != nullptr);

    if (add) {
        auto r = add->execute(nlohmann::json{{"a", 17}, {"b", 25}});
        CHECK(r.has_value());
        if (r) {
            std::printf("  add(17,25) -> %s\n", r->text.c_str());
            // The example server returns the sum 42 somewhere in its text.
            CHECK(r->text.find("42") != std::string::npos);
            // Structured output (structuredContent {"sum":42}) is preserved
            // and rendered as a JSON block.
            CHECK(r->text.find("\"sum\"") != std::string::npos);
        } else {
            std::fprintf(stderr, "  add() failed: %s\n", r.error().render().c_str());
        }
    }

    // ── untrusted annotations fail closed ────────────────────────────────
    // Server annotations are hints, not an enforcement boundary. Unless the
    // config explicitly sets trustAnnotations:true, even a claimed read-only
    // tool receives the conservative full effect set.
    if (add) {
        CHECK(add->effects.has(tools::Effect::ReadFs));
        CHECK(add->effects.has(tools::Effect::WriteFs));
        CHECK(add->effects.has(tools::Effect::Exec));
    }
    // `now` has no readOnlyHint → conservative full effect set (asks perms).
    const tools::ToolDef* now = nullptr;
    for (const auto& t : tools)
        if (t.name.value == "now" || t.name.value.find("__now") != std::string::npos) now = &t;
    if (now) CHECK(now->effects.has(tools::Effect::Exec));

    // ── resources ─────────────────────────────────────────────────────────
    auto resources = mcp::mcp_resources();
    std::printf("mcp_bridge_test: %zu resource(s)\n", resources.size());
    CHECK(!resources.empty());
    bool found_motd = false;
    for (const auto& r : resources) {
        std::printf("  resource: %s (%s)\n", r.uri.c_str(), r.title.c_str());
        if (r.uri.find("motd") != std::string::npos) found_motd = true;
    }
    CHECK(found_motd);
    {
        std::string err;
        auto body = mcp::mcp_read_resource("file:///motd", err);
        CHECK(body.has_value());
        if (body) {
            std::printf("  read motd -> %s\n", body->c_str());
            CHECK(body->find("Welcome") != std::string::npos);
        } else {
            std::fprintf(stderr, "  read_resource failed: %s\n", err.c_str());
        }
    }
    // The generic mcp_read_resource tool is present and lists resources.
    const tools::ToolDef* read_res = nullptr;
    for (const auto& t : tools) if (t.name.value == "mcp_read_resource") read_res = &t;
    CHECK(read_res != nullptr);
    if (read_res) {
        auto listing = read_res->execute(nlohmann::json::object());
        CHECK(listing.has_value());
        if (listing) CHECK(listing->text.find("motd") != std::string::npos);
    }

    // ── prompts ───────────────────────────────────────────────────────────
    auto prompts = mcp::mcp_prompts();
    std::printf("mcp_bridge_test: %zu prompt(s)\n", prompts.size());
    CHECK(!prompts.empty());
    bool found_summarize = false;
    for (const auto& p : prompts) {
        std::printf("  prompt: %s\n", p.name.c_str());
        if (p.name.find("summarize") != std::string::npos) found_summarize = true;
    }
    CHECK(found_summarize);
    {
        std::string err;
        auto rendered = mcp::mcp_get_prompt("summarize", {{"text", "hello world"}}, err);
        CHECK(rendered.has_value());
        if (rendered) {
            std::printf("  render summarize -> %s\n", rendered->c_str());
            CHECK(rendered->find("hello world") != std::string::npos);
        } else {
            std::fprintf(stderr, "  get_prompt failed: %s\n", err.c_str());
        }
    }
    const tools::ToolDef* get_prompt = nullptr;
    for (const auto& t : tools) if (t.name.value == "mcp_get_prompt") get_prompt = &t;
    CHECK(get_prompt != nullptr);

    // ── per-tool live exclude (the enable/disable toggle path) ───────────
    // Disabling a tool must drop it from the projected catalog WITHOUT a
    // re-spawn: project_tools reads the config's tools.exclude live, and
    // mcp_bump_generation() forces the next projection. Rewrite the config
    // to exclude "add", re-list, and confirm it's gone; then clear the
    // exclude and confirm it's back — proving the re-spawn-free toggle both
    // filters and un-filters, with no hang.
    {
        auto has_add = []{
            for (auto& t : mcp::mcp_tools_live())
                if (t.name.value.find("__add") != std::string::npos
                    || t.name.value == "add") return true;
            return false;
        };
        CHECK(has_add());                       // present before toggle
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\", \"tools\": { \"exclude\": [\"add\"] } } } }\n";
        }
        mcp::mcp_bump_generation();
        CHECK(!has_add());                      // disabled → filtered out
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\" } } }\n";
        }
        mcp::mcp_bump_generation();
        CHECK(has_add());                       // re-enabled → back, no hang
        std::printf("mcp_bridge_test: per-tool live exclude toggles cleanly\n");

        // Anti-vanishing: mcp_server_tools lists ALL advertised tools
        // regardless of exclude, so the picker can always show a disabled
        // tool (to re-enable it). Disable "add", then confirm it's STILL
        // in the unfiltered server-tools list.
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\", \"tools\": { \"exclude\": [\"add\"] } } } }\n";
        }
        mcp::mcp_bump_generation();
        auto all = mcp::mcp_server_tools("demo");
        bool add_listed = false;
        for (auto& n : all) if (n == "add") add_listed = true;
        CHECK(add_listed);   // disabled tool still enumerable → never vanishes
        // restore
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\" } } }\n";
        }
        mcp::mcp_bump_generation();

        // ── plugin_model: the unified source of truth ────────────────────
        // Disable "add"; the model must still LIST it (never vanish), mark
        // it disabled, and report the right enabled count. Then re-enable
        // and confirm it flips back. This is the authoritative view the
        // picker renders.
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\", \"tools\": { \"exclude\": [\"add\"] } } } }\n";
        }
        mcp::mcp_bump_generation();
        {
            auto pm = mcp::plugin_model();
            const mcp::ServerState* demo = nullptr;
            for (auto& sv : pm.servers) if (sv.name == "demo") demo = &sv;
            CHECK(demo != nullptr);
            if (demo) {
                CHECK(demo->connected);
                bool add_listed = false, add_enabled = true;
                for (auto& t : demo->tools)
                    if (t.name == "add") { add_listed = true; add_enabled = t.enabled; }
                CHECK(add_listed);        // still in the model (no vanish)
                CHECK(!add_enabled);      // correctly marked disabled
                CHECK(demo->tools.size() >= 2);  // add + now still there
            }
        }
        {
            std::ofstream f(cfg, std::ios::trunc);
            f << "{ \"mcpServers\": { \"demo\": { \"command\": \""
              << server << "\" } } }\n";
        }
        mcp::mcp_bump_generation();
        {
            auto pm = mcp::plugin_model();
            const mcp::ServerState* demo = nullptr;
            for (auto& sv : pm.servers) if (sv.name == "demo") demo = &sv;
            CHECK(demo != nullptr);
            if (demo) {
                bool add_enabled = false;
                for (auto& t : demo->tools) if (t.name == "add") add_enabled = t.enabled;
                CHECK(add_enabled);       // re-enabled
            }
        }
        std::printf("mcp_bridge_test: plugin_model unifies config + live cleanly\n");
    }

    if (g_failures == 0) { std::printf("mcp_bridge_test: all checks passed\n"); return 0; }
    std::fprintf(stderr, "mcp_bridge_test: %d failure(s)\n", g_failures);
    return 1;
}
