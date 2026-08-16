#pragma once
// agentty::mcp::PluginModel — the value snapshot of plugin/tool state.
//
// Split out of client.hpp so it can live IN the TEA Model (model.hpp) without
// dragging in the whole MCP client API (which depends on tools::ToolDef and
// would form an include cycle). This is a plain data type — <string>/<vector>
// only — precisely so the Model can own it and the view/visual_hash can treat
// the Plugins panel as a pure function of the Model. See docs/design/
// plugin-model.md and docs/design/plugin-model-in-model.md.

#include <cstddef>
#include <string>
#include <vector>

namespace agentty::mcp {

// One advertised tool of a connected server.
struct ToolState {
    std::string name;            // bare advertised name (no mcp__ prefix)
    std::string description;     // advertised description (for the UI)
    bool        enabled = true;  // NOT in config tools.exclude
    bool        over_budget = false; // enabled but trimmed from the wire
};

// One configured MCP server + its live connection state.
struct ServerState {
    std::string name;            // config key
    std::string command;         // config command
    bool        connected = false;   // handshake succeeded this session
    std::string error;           // why not connected (empty if connected/ok)
    std::vector<ToolState> tools;

    [[nodiscard]] std::size_t enabled_count() const noexcept {
        std::size_t n = 0;
        for (const auto& t : tools) if (t.enabled) ++n;
        return n;
    }
};

// The single UI-facing truth: every configured server, its connection state,
// and its advertised tools, unified from config + live pool. A value snapshot
// safe to hold across a concurrent pool swap.
struct PluginModel {
    std::vector<ServerState> servers;
    std::size_t native_tool_count = 0; // agentty's own tools (always shipped)
    std::size_t wire_tool_count   = 0; // total tools actually on the wire
    std::size_t tool_budget       = 0; // soft cap (0 = unset)
    std::size_t trimmed_count     = 0; // enabled MCP tools dropped for budget

    [[nodiscard]] bool over_budget() const noexcept {
        return tool_budget > 0 && wire_tool_count > tool_budget;
    }
    [[nodiscard]] std::size_t trimmed() const noexcept { return trimmed_count; }
};

} // namespace agentty::mcp
