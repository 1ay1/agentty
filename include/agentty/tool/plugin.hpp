#pragma once
// agentty::tools::plugin — the MCP plugin manager (`agentty plugin …`).
//
// A "plugin" in agentty IS an MCP server entry in mcp.json — there is no
// separate plugin runtime, registry, or manifest format. This module is
// the thin ergonomic layer over that fact: it edits mcp.json cleanly so
// users never hand-write JSON, with recipes for the common runtimes:
//
//   agentty plugin add weather --uvx mcp-weather          # PyPI, via uv
//   agentty plugin add mytool --python tools/my_mcp.py    # local script
//   agentty plugin add fs --npx @modelcontextprotocol/server-filesystem
//   agentty plugin add custom -- ./bin/server --flag x    # raw argv
//   agentty plugin list
//   agentty plugin remove weather
//
// Scope: --user (default) edits ~/.agentty/mcp.json; --project edits
// ./.agentty/mcp.json. Project configs additionally require the existing
// AGENTTY_MCP_ALLOW_PROJECT=1 trust gate before agentty will CONNECT to
// them (writing the entry is always allowed; the gate protects against
// cloned-repo configs running code, not against the user's own edits).
//
// Editing contract (pinned by plugin_config_test):
//   • Round-trip safe: every key this module does not own (other servers,
//     "servers" spelling, unknown top-level keys, per-server env/url/…)
//     is preserved byte-for-byte in JSON value terms.
//   • add on an existing name fails unless --force (no silent clobber).
//   • remove of an absent name is a distinct "not found" outcome.
//   • A missing file is created (with parent dirs) on first add.

#include <filesystem>
#include <string>
#include <vector>

namespace agentty::tools::plugin {

struct ServerSpec {
    std::string              name;
    std::string              command;
    std::vector<std::string> args;
};

enum class EditResult : std::uint8_t {
    Ok,
    AlreadyExists,   // add without --force onto an existing name
    NotFound,        // remove of an absent name
    ParseError,      // existing file is not valid JSON — refuse to touch it
    IoError,         // cannot read/write the path
};

// Add `spec` to the mcpServers object of the JSON file at `path`,
// creating the file if absent. Preserves every other key. `force`
// overwrites an existing entry of the same name.
[[nodiscard]] EditResult add_server(const std::filesystem::path& path,
                                    const ServerSpec& spec, bool force);

// Remove the named server. Preserves everything else.
[[nodiscard]] EditResult remove_server(const std::filesystem::path& path,
                                       const std::string& name);

// Enable/disable a WHOLE server without removing it — persisted as the
// server's top-level `disabled` flag in mcp.json (the bridge already skips a
// disabled server on connect). This is the primary Enter action on a plugin
// row: a reversible on/off, distinct from the destructive remove. No-op-Ok if
// already in the desired state.
[[nodiscard]] EditResult set_server_disabled(const std::filesystem::path& path,
                                             const std::string& name,
                                             bool disabled);

// True when the named server carries `"disabled": true` in mcp.json.
[[nodiscard]] bool is_server_disabled(const std::filesystem::path& path,
                                      const std::string& name);

// List the servers in the file (empty on missing/invalid file).
[[nodiscard]] std::vector<ServerSpec>
list_servers(const std::filesystem::path& path);

// Enable/disable ONE tool of a server, persisted as the server's
// `tools.exclude` list in mcp.json (the bridge already honours it). A
// disabled tool is dropped from the wire catalog on the next reload.
// `bare` is the tool's short name (e.g. "current_date", NOT the
// mcp__server__tool form). No-op-Ok if already in the desired state.
[[nodiscard]] EditResult set_tool_enabled(const std::filesystem::path& path,
                                          const std::string& server,
                                          const std::string& bare,
                                          bool enabled);

// True when `bare` is in server's tools.exclude (i.e. disabled).
[[nodiscard]] bool is_tool_disabled(const std::filesystem::path& path,
                                    const std::string& server,
                                    const std::string& bare);

// The bare names in a server's tools.exclude list (its disabled tools).
// Empty if the server/list is absent. Lets the UI show disabled tools
// (which are dropped from the live pool, so invisible otherwise).
[[nodiscard]] std::vector<std::string>
disabled_tools(const std::filesystem::path& path, const std::string& server);

// The config path for a scope. user → ~/.agentty/mcp.json,
// project → ./.agentty/mcp.json.
[[nodiscard]] std::filesystem::path config_path(bool project);

// The `agentty plugin` CLI: verb ∈ {add, remove, list} with the argv tail
// after the verb. Returns a process exit code. Prints results/errors and,
// after a successful add, a short "restart to connect / trust gate" note.
int cli(const std::vector<std::string>& argv);

} // namespace agentty::tools::plugin
