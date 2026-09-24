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
