#pragma once
// agentty::mcp — the SERVER side: expose agentty's own native tools OVER MCP.
//
// The inverse of client.hpp. client.hpp lets agentty CONSUME a remote MCP
// server's tools as local ToolDefs; this lets agentty BE an MCP server, so any
// MCP client (Claude Desktop, another agent, an IDE) can call agentty's native
// tools — `read`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `web_fetch`,
// `web_search`, `find_definition`, `diagnostics`, the git_* family, … — over a
// standard transport.
//
//   ┌──────────────┐   tools/list           ┌───────────────────────────┐
//   │  MCP client  │ ─────────────────────► │  agentty mcp-serve        │
//   │ (Claude/IDE) │   tools/call read {…}   │   tools::registry()       │
//   │              │ ◄───────────────────── │   → DynamicDispatch::exec  │
//   └──────────────┘   CallToolResult        └───────────────────────────┘
//
// This is the ONE honest place to put "all agentty tools through MCP": the
// process boundary is real, so JSON is the real contract. The internal agent
// loop keeps its fast in-process ToolDef path (FileChange round-trips natively
// there); a remote caller simply receives the diff as text, which is correct
// for that boundary.
//
// PERF CONTRACT: this header is mcp-cpp-FREE (no template cost leaks). All the
// heavy machinery lives in src/mcp/serve.cpp, compiled into the agentty_mcp
// object library like the rest of the bridge. The entry point only runs when
// the user invokes `agentty mcp-serve`; the normal TUI/ACP paths never touch
// it, so cold start is unaffected.

namespace agentty::mcp {

// Run agentty as an MCP server over stdio (newline-delimited JSON-RPC on
// stdin/stdout; diagnostics on stderr so the protocol channel stays clean).
// Registers every tool in tools::registry() and blocks serving requests until
// stdin closes. Returns a process exit code. The provider/subagent/sandbox
// seams must already be wired by the caller (main.cpp does this) so tools like
// `task` and `bash` behave exactly as they do in the TUI.
[[nodiscard]] int serve_stdio();

} // namespace agentty::mcp
