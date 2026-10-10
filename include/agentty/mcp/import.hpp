#pragma once
// agentty::mcp::import_ — adopt another tool's MCP servers into ours.
//
// ── The issue this answers, and why it is not a read root ────────────────
//
// #60: "Read ~/.ai/mcp/mcp.json for MCP servers — this is a global path used
// by Jetbrains IDEs, and it would be nice to have all my custom MCP servers
// in one place."
//
// The literal ask is a sixth read location. The actual wish is the opposite
// of that, and the distinction is the whole design:
//
//   If agentty READ that file, the user's servers would live in TWO places
//   with a precedence rule between them. They asked for ONE place.
//
// So the feature that sounds like the request would deliver the inverse of
// it. Import gives them exactly what they said: after `agentty mcp import`,
// every server is in ~/.agentty/mcp.json and there is nothing else to track.
//
// ── The rule that decides read-vs-import, for anything foreign ───────────
//
//     Does acting on this content EXECUTE something?
//       no  → read it live. staleness is the only risk.
//       yes → import it. show a diff, write it down, own it.
//
// Instructions, rules and skills are data: reading another tool's copy live
// costs nothing worse than being out of date. `mcp.json` spawns processes.
// Live-reading a file we do not own, cannot validate, and whose edits we
// never observe would let ANOTHER TOOL'S config change what we execute —
// silently, because nothing tells us it changed. That is a trust boundary,
// not a path lookup.
//
// Import also collapses the schema problem into one place. Foreign configs
// vary in path, nesting AND spelling; a read root would need a per-dialect
// leaf and a per-dialect parser, at which point scope::plan stops being the
// Locus × Dialect product and becomes a hand-written list of paths with tags
// on them. That list is precisely what the product was invented to kill.
//
// ── On the path in the issue title ───────────────────────────────────────
//
// There is no ~/.ai/mcp/mcp.json. JetBrains' own docs put Junie's user-scope
// config at ~/.junie/mcp/mcp.json (project scope: .junie/mcp/mcp.json), and
// the schema is the same `mcpServers` object agentty already speaks. The
// reporter remembered the shape and not the directory — which is exactly why
// `agentty config mcp` now PRINTS the sources it does not read, with the one
// command that adopts them.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "agentty/util/io.hpp"

namespace agentty::mcp::import_ {

namespace fs = std::filesystem;

// A tool whose MCP config agentty can adopt.
//
// This is a registry of FOREIGN shapes, deliberately kept out of
// agentty::scope. scope's axes (Locus × Dialect) describe OUR layout, where
// only the directory name varies; these vary in nesting and file name too,
// so modelling them there would break the product. Here they are just data.
enum class Tool : std::uint8_t {
    Junie,      // ~/.junie/mcp/mcp.json      · .junie/mcp/mcp.json
    ClaudeCode, // ~/.claude.json (nested)    · ./.mcp.json
    Cursor,     // ~/.cursor/mcp.json         · .cursor/mcp.json
    VsCode,     // —                          · .vscode/mcp.json
};

[[nodiscard]] std::string_view name_of(Tool) noexcept;
[[nodiscard]] std::vector<Tool> all_tools();

// Parse `name` as a tool id ("junie", "claude", "cursor", "vscode").
[[nodiscard]] bool parse_tool(std::string_view, Tool& out) noexcept;

// Where a tool keeps its config. Both may be empty if the tool has no such
// scope (VS Code has no documented user-level mcp.json).
struct Locations {
    fs::path user;
    fs::path project;   // relative to the project root
};
[[nodiscard]] Locations locations_of(Tool, const fs::path& home,
                                     const fs::path& project);

// One server found in a foreign config.
//
// `command`/`url` are decoded only so the scan can SHOW you what it found and
// reject an entry with no transport. The import itself copies `entry`
// verbatim -- `mcp.json` is one schema across tools, so an env block, custom
// headers or a timeout all survive. Translating field-by-field is how an
// import silently loses the API key that made the server work.
struct Found {
    std::string name;
    std::string command;     // for display + validation
    std::string url;         // for display + validation
    nlohmann::json entry;    // the WHOLE source object, copied as-is
    fs::path    from;        // the file it was read from
    // Set when agentty already has a server of this name. Import never
    // silently replaces one: a name collision is the user's call, and
    // guessing wrong would swap the command under an approved name — the
    // MCPoison shape, arriving through the front door.
    bool        conflicts = false;
};

// Everything a tool's configs offer, across both scopes.
struct Scan {
    Tool                 tool{};
    std::vector<fs::path> looked_in;   // every path probed, present or not
    std::vector<Found>    servers;
    std::string           error;       // malformed config: refuse, don't guess
    // Entries that were present but not importable: no command or url, or a
    // name agentty cannot manage. Counted so the scan can SAY so -- "3 of 5
    // imported" with no explanation is how someone concludes it is broken.
    int                   skipped = 0;
};

// Read a tool's configs. Pure-ish: touches only the filesystem, writes
// nothing. A missing file is not an error (it yields no servers).
[[nodiscard]] Scan scan(Tool, const fs::path& home, const fs::path& project);

// Scan every known tool, skipping those with nothing to offer.
[[nodiscard]] std::vector<Scan> scan_all(const fs::path& home,
                                         const fs::path& project);

struct Options {
    bool dry_run = false;   // print what would happen, touch nothing
    bool force   = false;   // adopt conflicting names too (replaces ours)
};

struct Outcome {
    int imported  = 0;
    int skipped   = 0;      // conflicts left alone
    int failed    = 0;
    fs::path into;          // the file written
};

// Adopt `servers` into agentty's USER config (~/.agentty/mcp.json).
//
// User scope on purpose: an imported server came from the human's own global
// tool config, so it is theirs, not the repo's — writing it into a project
// file would commit someone's personal setup into version control, and would
// also land it in the locus that needs vouching to run.
[[nodiscard]] Outcome adopt(Io, const std::vector<Found>&, const Options&);

// CLI: `agentty mcp import [--from <tool>] [--dry-run] [--force]`.
// With no --from, scans every known tool and reports what it finds.
int cli(Io, const std::vector<std::string>& argv);

}  // namespace agentty::mcp::import_
