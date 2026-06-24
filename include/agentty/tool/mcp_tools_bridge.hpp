#pragma once
// mcp_tools_bridge — adapts mcp-cpp's batteries-included toolset
// (mcp::tools::make_provider) back into agentty's native ToolDef surface.
//
//   The Tier-1 tool *implementations* now live in mcp-cpp (read/write/edit/
//   list_dir, bash, grep/glob/find_definition, diagnostics, git_*, web_*).
//   agentty consumes them through a cap::CapabilityProvider and re-wraps each
//   advertised tool as a ToolDef whose execute() closure dispatches into the
//   provider and decodes the `_mcp_tools` meta (effects + FileChange) back
//   into agentty's ToolOutput. The diff-review UI, changes-strip, and
//   permission policy keep seeing exactly the types they always did.
//
//   Host-coupled tools (remember/forget/wipe, todo, skill, search_docs, task)
//   are SHELLS owned by mcp-cpp; agentty injects their backends via
//   HostServices adapters built here.

#include <memory>
#include <vector>

#include "agentty/tool/registry.hpp"

namespace agentty::tools {

// Build the agentty-facing ToolDef list from the mcp-cpp provider. Called
// once by build_registry(). Constructs the HostServices adapters (memory/
// todo/skill/retriever/subagent/http), spins up make_provider(), keeps the
// provider alive for the process lifetime (its handlers are captured in the
// returned ToolDef::execute closures), and returns one ToolDef per advertised
// tool. Tool order follows the provider's list() order.
[[nodiscard]] std::vector<ToolDef> build_mcp_tool_defs();

} // namespace agentty::tools
