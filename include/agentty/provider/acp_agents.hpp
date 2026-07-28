#pragma once

// ── agentty::provider — external ACP agent registry (config surface) ─────────
//
// Maps a provider spec id (e.g. "claude-agent-acp", "codex-acp") to the
// argv/env/cwd needed to SPAWN that external ACP agent subprocess. This is the
// config half of the ExternalAcpBackend feature: registry.hpp knows a spec
// EXISTS and is Kind::ExternalAcp; this file knows HOW to launch it.
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
//
// BUILT-IN DEFAULTS: even with no config file, the well-known agents resolve to
// a sensible default argv (just their binary name on $PATH), so a user who has
// `claude-agent-acp` installed can select it with zero config. A config entry
// for the same id OVERRIDES the built-in default (e.g. to pass extra args or a
// pinned absolute path).

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

// Resolve the launch spec for a given provider spec id. Consults the config
// file first (if present + trusted), then falls back to a built-in default for
// well-known agents. Returns std::nullopt when the id is unknown AND has no
// built-in default (so the caller can surface a clear error).
[[nodiscard]] std::optional<AcpAgentSpec> resolve_acp_agent(std::string_view id);

// True when a spec id names an external ACP agent this build can launch —
// either a built-in default or a config entry. Used by parse_selection to
// route the spec to Kind::ExternalAcp.
[[nodiscard]] bool is_acp_agent_id(std::string_view id) noexcept;

} // namespace agentty::provider
