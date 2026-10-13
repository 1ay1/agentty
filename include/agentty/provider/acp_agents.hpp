#pragma once

// ── agentty::provider — external ACP agent registry (config surface) ─────────
//
// Maps a provider spec id (any explicitly config-defined id) to the argv/env/
// cwd needed to SPAWN that external ACP agent subprocess. This is the config
// half of the ExternalAcpBackend feature: registry.hpp knows a spec EXISTS and
// is Kind::ExternalAcp; this file knows HOW to launch it.
//
// This is agentty's equivalent of Zed's `agent_servers` config: the ACP
// mechanism is GENERIC — it drives any external ACP agent subprocess you name.
// There are no built-ins or per-agent hardcoded rows. You register agents by
// name in acp-agents.json, exactly as you would in Zed.
//
// Resolution order (first hit wins), mirroring .agentty/mcp.json:
//   1. $AGENTTY_ACP_AGENTS  — explicit path to a JSON file (trusted).
//   2. ~/.agentty/acp-agents.json  — user-global (trusted).
//   3. ./.agentty/acp-agents.json  — workspace-local (gated: can spawn
//      arbitrary commands, so requires AGENTTY_ACP_ALLOW_PROJECT=1).
//
// JSON shape (object keyed by spec id):
//   {
//     "acpAgents": {
//       "claude-agent-acp": {
//         "command": "claude-agent-acp",
//         "args": ["--stdio"],
//         "env": { "FOO": "bar" },
//         "cwd": "/optional/working/dir"
//       }
//     }
//   }
// ("agents" is accepted as an alias for "acpAgents".)
// With no config file, the registry is empty and no external process is
// selectable.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentty::provider {

// A fully-resolved launch spec for one external ACP agent.
struct AcpAgentSpec {
    std::string              id;       // canonical spec id (matches the registry row)
    std::string              command;  // argv[0] — binary name or absolute path
    std::vector<std::string> args;     // argv[1..]
    std::vector<std::pair<std::string, std::string>> env;  // extra environment
    std::string              cwd;      // working dir ("" = inherit process cwd)

    // The full argv (command + args) for spawn_acp_agent().
    [[nodiscard]] std::vector<std::string> argv() const {
        std::vector<std::string> v;
        v.reserve(args.size() + 1);
        v.push_back(command);
        for (const auto& a : args) v.push_back(a);
        return v;
    }
};

// Resolve the launch spec for a configured provider spec id. Returns
// std::nullopt when the id is not explicitly configured.
[[nodiscard]] std::optional<AcpAgentSpec> resolve_acp_agent(std::string_view id);

// True when a spec id names a configured external ACP agent. Used by
// parse_selection to route the spec to Kind::ExternalAcp.
[[nodiscard]] bool is_acp_agent_id(std::string_view id) noexcept;

// Enumerate configured ACP agents for dynamic provider-picker rows. Empty
// config means an empty result.
[[nodiscard]] std::vector<AcpAgentSpec> enumerate_acp_agents();

} // namespace agentty::provider
