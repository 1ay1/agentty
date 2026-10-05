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
    CHECK(stdio->entry["args"].size() == 2);
    CHECK(stdio->entry["args"][1] == "@jetbrains/mcp-proxy");

    const auto* remote = by_name(s, "remote-one");
    REQUIRE(remote != nullptr, "remote server found");
    CHECK(remote->url == "https://api.example.com/mcp/");
    CHECK(remote->entry["type"] == "http");

    // A server with neither a command nor a url cannot be connected to.
    // Importing it would produce an entry that fails later with no
    // explanation; skipping it is the honest outcome.
    CHECK(by_name(s, "neither") == nullptr,
          "an entry with no transport is not imported");
}

TEST_CASE("import is lossless — every key survives") {
    // THE failure mode this design exists to avoid. `mcp.json` is ONE schema
    // shared across tools, and agentty's own connect path reads `env`,
    // `headers`, `timeoutMs`, `connectTimeoutMs` and `tools.exclude`. An
    // import that rebuilt the entry from the handful of fields it bothered to
    // name would drop the API key that made the server work — and the server
    // would import looking complete, then fail at spawn with an auth error
    // that names nothing.
    //
    // So import copies the entry verbatim. This pins that.
    const auto dir = sandbox("lossless");
    write_file(dir / ".junie" / "mcp" / "mcp.json", R"({"mcpServers":{
        "rich": {
            "command": "/opt/server",
            "args": ["--port", "8080"],
            "env": {"API_KEY": "secret-value", "REGION": "eu"},
            "timeoutMs": 45000,
            "connectTimeoutMs": 9000,
            "tools": {"exclude": ["dangerous_tool"]},
            "someFutureKey": {"nested": true}
        }
    }})");

    const imp::Scan s = imp::scan(imp::Tool::Junie, dir, dir / "nowhere");
    const auto* rich = by_name(s, "rich");
    REQUIRE(rich != nullptr, "found it");

    // Everything agentty itself reads at connect time.
    CHECK(rich->entry["env"]["API_KEY"] == "secret-value",
          "the env block survives — this is the one that silently breaks");
    CHECK(rich->entry["env"]["REGION"] == "eu");
    CHECK(rich->entry["timeoutMs"] == 45000);
    CHECK(rich->entry["connectTimeoutMs"] == 9000);
    CHECK(rich->entry["tools"]["exclude"][0] == "dangerous_tool");

    // And a key NEITHER tool models today. Copying verbatim means a field
    // added to the format next year arrives intact without a code change,
    // which an allow-list of known fields could never do.
    CHECK(rich->entry["someFutureKey"]["nested"] == true,
          "an unrecognised key is carried, not dropped");
}

TEST_CASE("the ADOPTED file keeps every key, and never clobbers") {
    // The round trip, not just the scan: what actually lands in
    // ~/.agentty/mcp.json. adopt() writes through user_root(), so point that
    // at a sandbox.
    const auto dir = sandbox("adopt");
    const fs::path home = dir / "home";
    fs::create_directories(home);

    const std::string prev = std::getenv("AGENTTY_HOME")
                                 ? std::getenv("AGENTTY_HOME") : "";
    const bool had = std::getenv("AGENTTY_HOME") != nullptr;
    ::setenv("AGENTTY_HOME", home.string().c_str(), 1);

    imp::Found f;
    f.name    = "rich";
    f.command = "/opt/server";
    f.entry   = json::parse(R"({
        "command": "/opt/server",
        "env": {"API_KEY": "secret-value"},
        "timeoutMs": 45000
    })");

    const imp::Outcome o = imp::adopt({f}, {});
    CHECK(o.imported == 1, "one server adopted");

    json doc;
    {
        std::ifstream in(o.into);
        REQUIRE(in.good(), "the file was written");
        in >> doc;
    }
    const auto& got = doc["mcpServers"]["rich"];
    CHECK(got["command"] == "/opt/server");
    CHECK(got["env"]["API_KEY"] == "secret-value",
          "the env block is ON DISK, not just in the scan");
    CHECK(got["timeoutMs"] == 45000);

    // Re-adopting the same name without --force must not overwrite. Swapping
    // a command under an existing name is the MCPoison shape; arriving via
    // import does not make it safe.
    imp::Found evil = f;
    evil.entry["command"] = "/evil";
    evil.conflicts = true;
    const imp::Outcome o2 = imp::adopt({evil}, {});
    CHECK(o2.imported == 0 && o2.skipped == 1, "conflict skipped");
    {
        std::ifstream in(o.into);
        json after;
        in >> after;
        CHECK(after["mcpServers"]["rich"]["command"] == "/opt/server",
              "the original entry is untouched");
    }

    // --force replaces it wholesale, which is the documented escape hatch.
    imp::Options force;
    force.force = true;
    const imp::Outcome o3 = imp::adopt({evil}, force);
    CHECK(o3.imported == 1, "--force replaces");

    if (had) ::setenv("AGENTTY_HOME", prev.c_str(), 1);
    else     ::unsetenv("AGENTTY_HOME");
}

TEST_CASE("names agentty cannot manage are refused, and counted") {
    // Import is a trust boundary: the bytes come from a file another tool
    // wrote, so a name can be anything. It is not just a label -- it is the
    // key you type to disable a server, the prefix on every tool it exposes,
    // and a cell in three tables.
    //
    // Two shapes break that, found by feeding a hostile fixture in: an EMPTY
    // name renders as a blank row nobody can address, and a name containing
    // a NEWLINE splits `plugin list` across two lines so the table stops
    // parsing by eye. Neither is a security hole (canonical_mcp_name()
    // sanitises tool names downstream), but both produce an entry the user
    // cannot manage.
    const auto dir = sandbox("names");
    // Note the ESCAPED \n: a literal newline would be invalid JSON, and the
    // point is a name that parses fine and then breaks everything after it.
    write_file(dir / ".junie" / "mcp" / "mcp.json",
               "{\"mcpServers\":{"
               "  \"good\":      {\"command\":\"/bin/true\"},"
               "  \"\":          {\"command\":\"/bin/x\"},"
               "  \"two\\nlines\": {\"command\":\"/bin/y\"},"
               "  \"../../odd\": {\"command\":\"/bin/z\"}"
               "}}");

    const imp::Scan s = imp::scan(imp::Tool::Junie, dir, dir / "nowhere");
    CHECK(by_name(s, "good") != nullptr, "an ordinary name is kept");
    CHECK(by_name(s, "") == nullptr, "an empty name is refused");
    for (const auto& f : s.servers)
        CHECK(f.name.find('\n') == std::string::npos,
              "no control bytes survive into a name");

    // `../../odd` is just an odd STRING -- it is a JSON key, never joined
    // onto a path. Rejecting it would turn a cosmetic worry into refusing
    // real servers, so it is kept deliberately.
    CHECK(by_name(s, "../../odd") != nullptr,
          "a path-looking name is still just a name");

    // And the refusals are REPORTED. "2 of 4 imported" with no explanation
    // is how someone concludes the import is broken.
    CHECK(s.skipped == 2, "both unusable entries are counted");
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
