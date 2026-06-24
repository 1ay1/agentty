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
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

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

int main() {
    std::string server = find_server();
    if (server.empty()) {
        std::printf("mcp_bridge_test: SKIP (no example server built; "
                    "build mcp-cpp with -DMCP_BUILD_EXAMPLES=ON or set "
                    "AGENTTY_MCP_E2E_SERVER)\n");
        return 0;
    }

    // Write a temp mcp.json pointing at the example server, and aim the bridge
    // at it via AGENTTY_MCP_CONFIG.
    auto tmp = fs::temp_directory_path() / "agentty_mcp_e2e";
    std::error_code ec; fs::create_directories(tmp, ec);
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

    // ── tool annotations → effects ────────────────────────────────────────
    // The example `add` tool declares readOnlyHint:true, so the bridge maps
    // it to a permission-free (ReadFs|Net) effect set, NOT the full set.
    if (add) {
        CHECK(add->effects.has(tools::Effect::ReadFs));
        CHECK(!add->effects.has(tools::Effect::WriteFs));
        CHECK(!add->effects.has(tools::Effect::Exec));
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

    if (g_failures == 0) { std::printf("mcp_bridge_test: all checks passed\n"); return 0; }
    std::fprintf(stderr, "mcp_bridge_test: %d failure(s)\n", g_failures);
    return 1;
}
