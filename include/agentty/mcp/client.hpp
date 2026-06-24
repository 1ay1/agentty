#pragma once
// agentty::mcp — Model Context Protocol client integration.
//
// "MCP as a capability provider" (the architecture essay, §2 / §10): an MCP
// server's tools are exposed to the agent as ORDINARY agentty tools. The
// model sees them in the same flat tool list as `read`/`edit`/`bash` and
// CANNOT tell a remote MCP tool from a local one — that is the entire point.
// agentty's capability seam is already `tools::ToolDef` (name + input_schema
// + effects + an execute(json)->ExecResult closure); an MCP tool is just one
// more ToolDef whose execute() round-trips a `tools/call` over JSON-RPC to a
// spawned server process. Removing MCP changes nothing else — the seam is
// the registry, not MCP.
//
// PERF CONTRACT (load-bearing — agentty stays sub-ms cold start / ~9MB):
//   • LAZY + OPT-IN. With no `.agentty/mcp.json` (or AGENTTY_MCP_CONFIG),
//     mcp_tools() returns {} having done ZERO work — no process spawn, no
//     handshake — so a user who doesn't use MCP pays nothing at startup.
//   • The heavy, template-rich mcp-cpp headers are confined to the .cpp
//     files (src/mcp/*.cpp); this header is mcp-cpp-FREE so its instantiation
//     cost never leaks into the rest of the tree. Everything below is a
//     forward-declared handle or a plain ToolDef.
//   • Synchronous call path: a tool runs on a cmd worker thread and blocks on
//     std::future::get() with a timeout — no coroutine runtime, no event loop
//     grafted onto the render/stream loop.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentty/tool/registry.hpp"   // ToolDef

namespace agentty::mcp {

// Opaque, reference-counted handle keeping all connected MCP servers alive
// for the lifetime of the process. The ToolDef execute() closures returned by
// mcp_tools() capture a copy of this (a shared_ptr), so the connections —
// each owning a spawned child process + its stdio transport + an mcp::Client
// — survive exactly as long as some tool can still be invoked. Defined in
// src/mcp/bridge.cpp; callers treat it as a token they hold but never inspect.
struct ConnectionPool;
using PoolHandle = std::shared_ptr<ConnectionPool>;

// Connect to every MCP server named in the config and synthesize a ToolDef
// for each tool each server advertises (via `initialize` + `tools/list`).
//
// Config resolution (first that exists wins; none → returns {} doing nothing):
//   1. $AGENTTY_MCP_CONFIG   (explicit path to a JSON file)
//   2. ./.agentty/mcp.json   (project-local)
//   3. ~/.agentty/mcp.json   (user-global)
//
// Config shape (Claude-Desktop-compatible):
//   {
//     "mcpServers": {
//       "github": { "command": "mcp-server-github", "args": ["--repo","x"],
//                   "env": { "TOKEN": "..." } },
//       "fs":     { "command": "npx", "args": ["-y","@mcp/server-filesystem","/tmp"] }
//     }
//   }
//
// Tool naming: remote tools are namespaced `mcp__<server>__<tool>` so two
// servers can expose a tool of the same name without collision and the user
// can see at a glance where a capability came from (provenance, essay §6).
//
// `out_pool` receives the keep-alive handle; store it somewhere with process
// lifetime (the registry does this). Never throws: a server that fails to
// spawn / handshake is skipped with a logged warning, and its tools are
// simply absent. The returned vector may be empty.
[[nodiscard]] std::vector<tools::ToolDef> mcp_tools(PoolHandle& out_pool);

// True when an MCP config file is present (cheap stat, no connection). Lets
// the registry decide whether to even attempt mcp_tools(). Pure I/O probe.
[[nodiscard]] bool mcp_config_present();

// ── Dynamic tool surface (tools/list_changed) ────────────────────────────
// MCP servers may emit `notifications/tools/list_changed` at any time. The
// connection pool keeps a LIVE snapshot of the synthesized ToolDefs, rebuilt
// whenever any server's tool list changes. `mcp_tools_live()` returns the
// current snapshot for the process-wide pool (the one mcp_tools() populated).
// Empty when MCP is unconfigured or nothing is connected.
//
// `mcp_generation()` bumps every time the snapshot changes; a caller that
// caches the tool list can compare generations to know when to re-pull. Both
// are O(1) and lock-guarded — safe to poll from the render/turn loop.
[[nodiscard]] std::vector<tools::ToolDef> mcp_tools_live();
[[nodiscard]] unsigned long mcp_generation() noexcept;

// ── Resources (MCP resources/*) ──────────────────────────────────────────
// A resource the agent can read by URI (file, db row, API doc, …).
struct ResourceInfo {
    std::string uri;
    std::string name;
    std::string title;        // human label (falls back to name)
    std::string description;
    std::string mime_type;
    std::string server;       // origin ("mcp:github")
};
[[nodiscard]] std::vector<ResourceInfo> mcp_resources();
// Read a resource. On success returns the flattened text (text contents
// concatenated; blob contents summarised as "[blob <mime> <bytes>B]").
// On failure the optional is empty and `err` carries a human message.
[[nodiscard]] std::optional<std::string> mcp_read_resource(const std::string& uri, std::string& err);

// ── Prompts (MCP prompts/*) ──────────────────────────────────────────────
struct PromptArgInfo { std::string name; std::string description; bool required = false; };
struct PromptInfo {
    std::string name;          // exposed (possibly namespaced) name
    std::string title;
    std::string description;
    std::vector<PromptArgInfo> arguments;
    std::string server;
};
[[nodiscard]] std::vector<PromptInfo> mcp_prompts();
// Render a prompt to a flattened string (role-tagged messages joined). `args`
// is a name→value map for the prompt's template arguments. On failure returns
// empty optional + `err`.
[[nodiscard]] std::optional<std::string> mcp_get_prompt(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& args,
    std::string& err);

} // namespace agentty::mcp
