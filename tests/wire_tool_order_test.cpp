// select_wire_tools_from: which tools go on the wire, and in what order.
//
// Natives and pinned tools always go. Other (MCP) tools are capped by a query
// relevance score. The SCORE picks the set; the ORDER on the wire is catalog
// order. The tools block is the head of the prompt-cache prefix, so emitting
// in score order reordered it on every user message even when the chosen set
// was identical, and every turn paid full input price.

#include "agtest.hpp"

#include "agentty/tool/registry.hpp"

#include <string>
#include <vector>

using agentty::ToolName;
namespace tools = agentty::tools;

namespace {

tools::ToolDef mcp_tool(std::string name, std::string desc) {
    tools::ToolDef t;
    t.name = ToolName{std::move(name)};
    t.description = std::move(desc);
    t.origin = tools::ToolOrigin::Mcp;
    t.origin_id = "srv";
    return t;
}

std::vector<std::string> names(const std::vector<const tools::ToolDef*>& v) {
    std::vector<std::string> out;
    for (const auto* t : v) out.push_back(t->name.value);
    return out;
}

}  // namespace

TEST_CASE("wire tool order is catalog order, not score order") {
    std::vector<tools::ToolDef> cat;
    tools::ToolDef native;
    native.name = ToolName{"read"};
    cat.push_back(native);
    cat.push_back(mcp_tool("alpha_tickets", "jira tickets"));
    cat.push_back(mcp_tool("beta_pages", "confluence pages"));
    cat.push_back(mcp_tool("gamma_logs", "datadog logs"));

    // Two different queries that pick the SAME set but rank it differently.
    const auto a = names(tools::select_wire_tools_from(cat, "logs pages tickets", 16));
    const auto b = names(tools::select_wire_tools_from(cat, "tickets tickets pages logs", 16));
    check(a == b, "same set, same bytes, regardless of which tool scored highest");
    check(a.front() == "read", "natives first");

    // When the cap cuts, the kept tools still come out in catalog order.
    const auto c = names(tools::select_wire_tools_from(cat, "logs logs pages", 2));
    check(c.size() == 3, "native + 2 external");
    check(c[1] == "beta_pages" && c[2] == "gamma_logs",
          "kept pair in catalog order, not by score");
}

// Settings → Web Search can switch web_search off. It must leave the wire
// WITHOUT disturbing the order of what remains (the tools block is the cache
// prefix), and it must only ever remove the NATIVE tool — an MCP server that
// happens to expose a tool named web_search is the user's own choice and is
// governed by mcp.json, not by this switch.
TEST_CASE("web_search switch removes only the native tool, order intact") {
    std::vector<tools::ToolDef> cat;
    for (const char* n : {"read", "web_fetch", "web_search", "grep"}) {
        tools::ToolDef t;
        t.name = ToolName{n};
        cat.push_back(t);
    }
    cat.push_back(mcp_tool("web_search", "a plugin's own search"));

    const auto on  = names(tools::select_wire_tools_from(cat, "", 16, true));
    const auto off = names(tools::select_wire_tools_from(cat, "", 16, false));

    check(on == std::vector<std::string>{"read", "web_fetch", "web_search",
                                         "grep", "web_search"},
          "on: everything ships, catalog order");
    check(off == std::vector<std::string>{"read", "web_fetch", "grep",
                                          "web_search"},
          "off: the native web_search is gone, the rest keep their order, "
          "and the MCP tool of the same name is untouched");

    // Stable across queries while the setting holds — the property that
    // keeps the prompt cache warm.
    check(names(tools::select_wire_tools_from(cat, "search the web", 16, false)) == off,
          "off is byte-stable whatever the user asked");
}

// `agentty mcp-serve` registers native_registry() minus withheld_by_policy():
// the SAME predicate the wire uses, so another MCP client is never offered a
// tool agentty's settings switched off for its own model. Pinned on the
// predicate because serve_stdio owns a stdio transport and cannot run here.
TEST_CASE("mcp-serve and the wire withhold exactly the same tools") {
    std::vector<tools::ToolDef> cat;
    for (const char* n : {"read", "web_fetch", "web_search", "grep"}) {
        tools::ToolDef t;
        t.name = ToolName{n};
        cat.push_back(t);
    }
    cat.push_back(mcp_tool("web_search", "a plugin's own search"));

    for (bool enabled : {true, false}) {
        std::vector<std::string> served;
        for (const auto& t : cat)
            if (!tools::withheld_by_policy(t, enabled)) served.push_back(t.name.value);
        check(served == names(tools::select_wire_tools_from(cat, "", 16, enabled)),
              "mcp-serve's list and the wire agree, on and off");
    }
    check(tools::withheld_by_policy(cat[2], false), "off withholds native web_search");
    check(!tools::withheld_by_policy(cat[2], true), "on/auto serve it");
    check(!tools::withheld_by_policy(cat[1], false), "web_fetch is never governed");
    check(!tools::withheld_by_policy(cat[4], false), "nor a plugin's tool of the same name");
}
