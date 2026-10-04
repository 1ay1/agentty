// mcp_import_test — adopting another tool's MCP servers (#60).
//
// THE DESIGN UNDER TEST
// =====================
// #60 asked agentty to READ ~/.ai/mcp/mcp.json. The literal ask is a sixth
// read location; the actual wish ("all my custom MCP servers in one place")
// is the opposite of that — reading it would leave the servers in TWO places
// with a precedence rule between them.
//
// So this is an import: scan a foreign config, translate it, write it into
// ~/.agentty/mcp.json, and afterwards there is exactly one file. These cases
// pin the properties that make that safe:
//
//   • translation is faithful across stdio AND remote transports
//   • scanning NEVER writes (discovery is not a mutation)
//   • a name agentty already has is left alone unless --force
//   • a malformed foreign config refuses rather than importing half of it
//   • Claude's per-project nesting yields THIS project's servers only

#include "agtest.hpp"

#include "agentty/mcp/import.hpp"
#include "agentty/tool/plugin.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace imp = agentty::mcp::import_;
using json = nlohmann::json;

namespace {

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

[[nodiscard]] const imp::Found* by_name(const imp::Scan& s, const char* n) {
    for (const auto& f : s.servers) if (f.name == n) return &f;
    return nullptr;
}

[[nodiscard]] fs::path sandbox(const char* leaf) {
    const auto dir = fs::temp_directory_path()
                   / ("agentty_import_" + std::string{leaf});
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("tool ids and their real locations") {
    // The path in the issue title does not exist. JetBrains documents Junie's
    // user config at ~/.junie/mcp/mcp.json (project: .junie/mcp/mcp.json);
    // the reporter remembered the shape, not the directory. Pinning the real
    // paths here is what stops that guess from drifting back in.
    const fs::path home{"/h"}, proj{"/p"};

    const auto junie = imp::locations_of(imp::Tool::Junie, home, proj);
    CHECK(junie.user == fs::path{"/h/.junie/mcp/mcp.json"});
    CHECK(junie.project == fs::path{"/p/.junie/mcp/mcp.json"});

    // Claude Code: one big user file with servers nested per project, plus
    // the portable .mcp.json at a repo root.
    const auto cc = imp::locations_of(imp::Tool::ClaudeCode, home, proj);
    CHECK(cc.user == fs::path{"/h/.claude.json"});
    CHECK(cc.project == fs::path{"/p/.mcp.json"});

    const auto cur = imp::locations_of(imp::Tool::Cursor, home, proj);
    CHECK(cur.user == fs::path{"/h/.cursor/mcp.json"});

    // VS Code has no documented user-level file. An empty path is the honest
    // answer; inventing one would make the scan report a location that can
    // never exist.
    const auto vsc = imp::locations_of(imp::Tool::VsCode, home, proj);
    CHECK(vsc.user.empty());
    CHECK(vsc.project == fs::path{"/p/.vscode/mcp.json"});

    imp::Tool t{};
    CHECK(imp::parse_tool("junie", t) && t == imp::Tool::Junie);
    CHECK(imp::parse_tool("jetbrains", t) && t == imp::Tool::Junie);
    CHECK(imp::parse_tool("claude", t) && t == imp::Tool::ClaudeCode);
    CHECK(!imp::parse_tool("emacs", t));
}

TEST_CASE("translation keeps both transports, and skips what is neither") {
    const auto dir = sandbox("translate");
    write_file(dir / ".junie" / "mcp" / "mcp.json", R"({"mcpServers":{
        "stdio-one": {"command":"npx","args":["-y","@jetbrains/mcp-proxy"]},
        "remote-one": {"url":"https://api.example.com/mcp/","type":"http"},
        "neither":    {"description":"no command, no url"}
    }})");

    const imp::Scan s = imp::scan(imp::Tool::Junie, dir, dir / "nowhere");
    CHECK(s.error.empty(), "a well-formed config is not an error");

    const auto* stdio = by_name(s, "stdio-one");
    REQUIRE(stdio != nullptr, "stdio server found");
    CHECK(stdio->command == "npx");
    CHECK(stdio->args.size() == 2);
    CHECK(stdio->args[1] == "@jetbrains/mcp-proxy");

    const auto* remote = by_name(s, "remote-one");
    REQUIRE(remote != nullptr, "remote server found");
    CHECK(remote->url == "https://api.example.com/mcp/");
    CHECK(remote->type == "http");

    // A server with neither a command nor a url cannot be connected to.
    // Importing it would produce an entry that fails later with no
    // explanation; skipping it is the honest outcome.
    CHECK(by_name(s, "neither") == nullptr,
          "an entry with no transport is not imported");
}

TEST_CASE("scanning writes nothing") {
    // Discovery is not a mutation. `agentty mcp import` with no --from is a
    // report, and a report that creates files is a trap.
    const auto dir = sandbox("readonly");
    write_file(dir / ".cursor" / "mcp.json",
               R"({"mcpServers":{"x":{"command":"/bin/true"}}})");
    const auto before = fs::last_write_time(dir / ".cursor" / "mcp.json");

    // Distinct home and project dirs: passing the same path for both would
    // make the user- and project-scope probes land on one file and find the
    // server twice, which measures the fixture rather than the code.
    const imp::Scan s = imp::scan(imp::Tool::Cursor, dir, dir / "proj");
    CHECK(s.servers.size() == 1, "found it");
    CHECK(fs::last_write_time(dir / ".cursor" / "mcp.json") == before,
          "the foreign config is not touched");
    CHECK(!fs::exists(dir / ".agentty"), "nothing of ours is created");
}

TEST_CASE("a malformed foreign config refuses rather than guessing") {
    // Importing half a config is worse than importing none: the user would
    // have a partial server list and no reason to suspect it.
    const auto dir = sandbox("malformed");
    write_file(dir / ".cursor" / "mcp.json", R"({"mcpServers":{broken)");
    const imp::Scan s = imp::scan(imp::Tool::Cursor, dir, dir / "proj");
    CHECK(!s.error.empty(), "the parse failure is reported");
    CHECK(s.servers.empty(), "and nothing is salvaged from it");
}

TEST_CASE("claude nesting yields THIS project only") {
    // ~/.claude.json holds every project's servers in one file. Adopting
    // another checkout's would be a genuine surprise, so the reach-in is
    // keyed on the project path.
    const auto dir = sandbox("claude");
    const fs::path mine = dir / "mine";
    write_file(dir / ".claude.json",
               json{{"projects",
                     {{mine.string(),
                       {{"mcpServers", {{"ours", {{"command", "/opt/ours"}}}}}}},
                      {"/somewhere/else",
                       {{"mcpServers", {{"theirs", {{"command", "/opt/theirs"}}}}}}}}}}
                   .dump());

    const imp::Scan s = imp::scan(imp::Tool::ClaudeCode, dir, mine);
    CHECK(by_name(s, "ours") != nullptr, "this project's server is found");
    CHECK(by_name(s, "theirs") == nullptr,
          "another project's server is NOT adopted");
}

TEST_CASE("every probed path is reported, present or not") {
    // The scan prints where it looked. A user whose config is somewhere
    // unexpected needs to see the paths that were checked, not just "nothing
    // found" — that is the difference between a dead end and a next step.
    const auto dir = sandbox("probed");
    const imp::Scan s = imp::scan(imp::Tool::Junie, dir, dir / "proj");
    CHECK(s.servers.empty(), "nothing is there");
    CHECK(s.looked_in.size() == 2, "but both scopes were probed");
    CHECK(s.error.empty(), "a missing file is not an error");
}

TEST_CASE("scan_all skips tools with nothing to offer") {
    const auto dir = sandbox("scanall");
    CHECK(imp::scan_all(dir, dir / "proj").empty(),
          "an empty machine yields no noise");
    write_file(dir / ".junie" / "mcp" / "mcp.json",
               R"({"mcpServers":{"only":{"command":"/bin/true"}}})");
    const auto found = imp::scan_all(dir, dir / "nowhere");
    CHECK(found.size() == 1, "exactly the tool that has something");
    CHECK(found.front().tool == imp::Tool::Junie);
}
