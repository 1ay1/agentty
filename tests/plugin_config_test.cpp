// plugin_config_test.cpp — the `agentty plugin` mcp.json editor
// (tools::plugin): the editing contract from plugin.hpp. The property that
// matters most is ROUND-TRIP SAFETY — `plugin add` must never destroy
// config the user wrote by hand (other servers' env blocks, the "servers"
// spelling, unknown top-level keys).

#include "agentty/tool/plugin.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace plug = agentty::tools::plugin;
using nlohmann::json;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::println("  FAIL: {}", msg); ++g_failed; }
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

[[nodiscard]] json read_json(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return json::parse(f, nullptr, /*throw=*/false);
}

void creates_file_on_first_add(const fs::path& dir) {
    std::println("--- creates_file_on_first_add ---");
    const fs::path cfg = dir / "fresh" / "mcp.json";
    auto r = plug::add_server(cfg, {"weather", "uvx", {"mcp-weather"}},
                              /*force=*/false);
    check(r == plug::EditResult::Ok, "add on a missing file succeeds");
    json j = read_json(cfg);
    check(j.is_object() && j.contains("mcpServers"),
          "new file uses the mcpServers spelling");
    check(j["mcpServers"]["weather"]["command"] == "uvx", "command stored");
    check(j["mcpServers"]["weather"]["args"] == json::array({"mcp-weather"}),
          "args stored");
    std::println("PASS\n");
}

void round_trip_preserves_foreign_keys(const fs::path& dir) {
    std::println("--- round_trip_preserves_foreign_keys ---");
    const fs::path cfg = dir / "rt" / "mcp.json";
    // A realistic hand-written config: another server with env + an
    // unknown top-level key.
    write_file(cfg, R"({
  "mcpServers": {
    "github": {
      "command": "mcp-server-github",
      "args": ["--repo", "x"],
      "env": { "GITHUB_TOKEN": "secret" }
    }
  },
  "my_custom_toplevel": { "keep": true }
})");
    auto r = plug::add_server(cfg, {"weather", "uvx", {"mcp-weather"}},
                              false);
    check(r == plug::EditResult::Ok, "add succeeds");
    json j = read_json(cfg);
    check(j["mcpServers"]["github"]["env"]["GITHUB_TOKEN"] == "secret",
          "another server's env block survives");
    check(j["mcpServers"]["github"]["args"] == json::array({"--repo", "x"}),
          "another server's args survive");
    check(j["my_custom_toplevel"]["keep"] == true,
          "unknown top-level key survives");
    check(j["mcpServers"].contains("weather"), "new entry landed");

    // Remove the new entry: github + the custom key must still be intact.
    r = plug::remove_server(cfg, "weather");
    check(r == plug::EditResult::Ok, "remove succeeds");
    j = read_json(cfg);
    check(!j["mcpServers"].contains("weather"), "entry removed");
    check(j["mcpServers"]["github"]["env"]["GITHUB_TOKEN"] == "secret",
          "remove also preserves foreign keys");
    std::println("PASS\n");
}

void honours_servers_spelling(const fs::path& dir) {
    std::println("--- honours_servers_spelling ---");
    const fs::path cfg = dir / "sp" / "mcp.json";
    write_file(cfg, R"({"servers": {"a": {"command": "x"}}})");
    auto r = plug::add_server(cfg, {"b", "y", {}}, false);
    check(r == plug::EditResult::Ok, "add succeeds");
    json j = read_json(cfg);
    check(j.contains("servers") && !j.contains("mcpServers"),
          "existing 'servers' spelling is kept (no split-brain)");
    check(j["servers"].contains("a") && j["servers"].contains("b"),
          "both entries under the one key");
    std::println("PASS\n");
}

void no_clobber_without_force(const fs::path& dir) {
    std::println("--- no_clobber_without_force ---");
    const fs::path cfg = dir / "cl" / "mcp.json";
    (void)plug::add_server(cfg, {"t", "cmd1", {}}, false);
    auto r = plug::add_server(cfg, {"t", "cmd2", {}}, false);
    check(r == plug::EditResult::AlreadyExists, "duplicate add refused");
    check(read_json(cfg)["mcpServers"]["t"]["command"] == "cmd1",
          "original untouched after refusal");
    r = plug::add_server(cfg, {"t", "cmd2", {}}, /*force=*/true);
    check(r == plug::EditResult::Ok, "--force overwrites");
    check(read_json(cfg)["mcpServers"]["t"]["command"] == "cmd2",
          "forced entry stored");
    std::println("PASS\n");
}

void distinct_not_found(const fs::path& dir) {
    std::println("--- distinct_not_found ---");
    const fs::path cfg = dir / "nf" / "mcp.json";
    check(plug::remove_server(cfg, "ghost") == plug::EditResult::NotFound,
          "remove on a missing FILE is NotFound");
    (void)plug::add_server(cfg, {"real", "x", {}}, false);
    check(plug::remove_server(cfg, "ghost") == plug::EditResult::NotFound,
          "remove of an absent NAME is NotFound");
    check(read_json(cfg)["mcpServers"].contains("real"),
          "failed remove left the file alone");
    std::println("PASS\n");
}

void refuses_broken_json(const fs::path& dir) {
    std::println("--- refuses_broken_json ---");
    const fs::path cfg = dir / "br" / "mcp.json";
    write_file(cfg, "{ this is not json");
    check(plug::add_server(cfg, {"x", "y", {}}, false)
              == plug::EditResult::ParseError,
          "add refuses to rewrite an unparseable file");
    check(plug::remove_server(cfg, "x") == plug::EditResult::ParseError,
          "remove refuses too");
    std::ifstream f(cfg);
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    check(body == "{ this is not json",
          "broken file byte-identical after refusals");
    std::println("PASS\n");
}

void list_reads_back(const fs::path& dir) {
    std::println("--- list_reads_back ---");
    const fs::path cfg = dir / "ls" / "mcp.json";
    (void)plug::add_server(cfg, {"a", "uvx", {"pkg-a"}}, false);
    (void)plug::add_server(cfg, {"b", "python3", {"/x/s.py", "--flag"}}, false);
    auto servers = plug::list_servers(cfg);
    check(servers.size() == 2, "two entries listed");
    bool found_b = false;
    for (const auto& s : servers)
        if (s.name == "b" && s.command == "python3" && s.args.size() == 2
            && s.args[1] == "--flag")
            found_b = true;
    check(found_b, "listed entry round-trips command+args");
    check(plug::list_servers(dir / "absent.json").empty(),
          "missing file lists empty");
    std::println("PASS\n");
}

} // namespace

void tool_enable_disable(const fs::path& dir) {
    std::println("--- tool_enable_disable ---");
    const fs::path cfg = dir / "tgl" / "mcp.json";
    (void)plug::add_server(cfg, {"date", "/x/date", {}}, false);
    // disable one tool
    check(plug::set_tool_enabled(cfg, "date", "current_date", false)
              == plug::EditResult::Ok, "disable succeeds");
    check(plug::is_tool_disabled(cfg, "date", "current_date"),
          "tool recorded as disabled");
    check(read_json(cfg)["mcpServers"]["date"]["tools"]["exclude"]
              == json::array({"current_date"}),
          "tools.exclude holds the bare name");
    // idempotent disable
    check(plug::set_tool_enabled(cfg, "date", "current_date", false)
              == plug::EditResult::Ok, "double-disable is Ok no-op");
    // re-enable clears it (and prunes empty tools object)
    check(plug::set_tool_enabled(cfg, "date", "current_date", true)
              == plug::EditResult::Ok, "re-enable succeeds");
    check(!plug::is_tool_disabled(cfg, "date", "current_date"),
          "tool no longer disabled");
    check(!read_json(cfg)["mcpServers"]["date"].contains("tools"),
          "empty tools object pruned on re-enable");
    // command survives all of it
    check(read_json(cfg)["mcpServers"]["date"]["command"] == "/x/date",
          "server command preserved across toggles");
    // toggling on a missing server is NotFound
    check(plug::set_tool_enabled(cfg, "ghost", "x", false)
              == plug::EditResult::NotFound, "toggle on absent server NotFound");
    std::println("PASS\n");
}

void server_enable_disable(const fs::path& dir) {
    std::println("--- server_enable_disable ---");
    const fs::path cfg = dir / "srv" / "mcp.json";
    (void)plug::add_server(cfg, {"date", "/x/date", {}}, false);
    check(!plug::is_server_disabled(cfg, "date"), "new server starts enabled");
    // Ecosystem portability: an added server is tagged type:stdio (the
    // emerging cross-client convention).
    check(read_json(cfg)["mcpServers"]["date"]["type"] == "stdio",
          "added server carries type:stdio");
    // disable the whole server
    check(plug::set_server_disabled(cfg, "date", true) == plug::EditResult::Ok,
          "disable server succeeds");
    check(plug::is_server_disabled(cfg, "date"), "server recorded disabled");
    check(read_json(cfg)["mcpServers"]["date"]["disabled"] == true,
          "disabled:true persisted");
    // idempotent
    check(plug::set_server_disabled(cfg, "date", true) == plug::EditResult::Ok,
          "double-disable is Ok no-op");
    // re-enable removes the flag (absent == enabled, clean file)
    check(plug::set_server_disabled(cfg, "date", false) == plug::EditResult::Ok,
          "re-enable succeeds");
    check(!plug::is_server_disabled(cfg, "date"), "server enabled again");
    check(!read_json(cfg)["mcpServers"]["date"].contains("disabled"),
          "disabled key removed on re-enable (not left as false)");
    check(read_json(cfg)["mcpServers"]["date"]["command"] == "/x/date",
          "command preserved across server toggles");
    check(plug::set_server_disabled(cfg, "ghost", true)
              == plug::EditResult::NotFound, "disable absent server NotFound");
    std::println("PASS\n");
}

// A hand-edited malformed `disabled` (string, not bool) must NOT throw —
// is_server_disabled and set_server_disabled must fail-open (treat as false).
void malformed_disabled_no_throw(const fs::path& dir) {
    std::println("--- malformed_disabled_no_throw ---");
    const fs::path cfg = dir / "bad" / "mcp.json";
    write_file(cfg,
        R"({"mcpServers":{"date":{"command":"/x","disabled":"true"}}})");
    // Reading a string `disabled` must not throw and reads as "not disabled".
    bool threw = false;
    try {
        check(!plug::is_server_disabled(cfg, "date"),
              "string disabled reads as not-disabled (fail-open)");
    } catch (...) { threw = true; }
    check(!threw, "is_server_disabled does not throw on a string flag");
    // Toggling it must succeed and REPLACE the bad value with a real bool.
    threw = false;
    try {
        check(plug::set_server_disabled(cfg, "date", true) == plug::EditResult::Ok,
              "toggle over a malformed flag succeeds");
    } catch (...) { threw = true; }
    check(!threw, "set_server_disabled does not throw on a string flag");
    check(read_json(cfg)["mcpServers"]["date"]["disabled"] == true,
          "malformed flag replaced by a real bool");
    std::println("PASS\n");
}

// A non-object server entry (e.g. `"date": "just a string"`) must not throw.
void non_object_entry_no_throw(const fs::path& dir) {
    std::println("--- non_object_entry_no_throw ---");
    const fs::path cfg = dir / "nonobj" / "mcp.json";
    write_file(cfg, R"({"mcpServers":{"date":"just a string"}})");
    bool threw = false;
    try {
        check(plug::set_server_disabled(cfg, "date", true)
                  == plug::EditResult::NotFound,
              "non-object entry → NotFound, no throw");
        check(!plug::is_server_disabled(cfg, "date"),
              "non-object entry is_server_disabled false");
    } catch (...) { threw = true; }
    check(!threw, "non-object entry never throws");
    std::println("PASS\n");
}

// Concurrent mutations on detached threads must not lose updates or corrupt
// the file (the mutation_mutex + unique temp guarantee).
void concurrent_mutations_safe(const fs::path& dir) {
    std::println("--- concurrent_mutations_safe ---");
    const fs::path cfg = dir / "conc" / "mcp.json";
    // Seed N servers.
    constexpr int N = 24;
    for (int i = 0; i < N; ++i)
        (void)plug::add_server(cfg, {"s" + std::to_string(i), "/x", {}}, false);

    // Hammer: each thread disables a distinct server concurrently.
    std::vector<std::thread> ts;
    for (int i = 0; i < N; ++i)
        ts.emplace_back([&, i]{
            (void)plug::set_server_disabled(cfg, "s" + std::to_string(i), true);
        });
    for (auto& t : ts) t.join();

    // The file must still be valid JSON — no torn/interleaved write.
    json j = read_json(cfg);
    check(!j.is_discarded(), "file is still valid JSON after concurrent writes");
    // EVERY disable must have landed (no lost update).
    int disabled = 0;
    for (int i = 0; i < N; ++i)
        if (plug::is_server_disabled(cfg, "s" + std::to_string(i))) ++disabled;
    check(disabled == N,
          "all " + std::to_string(N) + " concurrent disables landed (got "
          + std::to_string(disabled) + ")");
    std::println("PASS\n");
}

// A symlinked mcp.json must be written THROUGH (target updated), not replaced
// by a regular file (which would orphan the user's dotfile symlink).
void symlink_written_through(const fs::path& dir) {
    std::println("--- symlink_written_through ---");
    const fs::path real = dir / "sym" / "real.json";
    const fs::path link = dir / "sym" / "mcp.json";
    write_file(real, R"({"mcpServers":{}})");
    std::error_code ec;
    fs::create_symlink(real, link, ec);
    if (ec) { std::println("  (skip: symlinks unsupported here)\nPASS\n"); return; }

    check(plug::add_server(link, {"date", "/x", {}}, false) == plug::EditResult::Ok,
          "add via symlink succeeds");
    check(fs::is_symlink(link), "mcp.json is STILL a symlink after the write");
    check(read_json(real).contains("mcpServers")
          && read_json(real)["mcpServers"].contains("date"),
          "the write landed in the symlink TARGET");
    std::println("PASS\n");
}

// Scope edit-routing: an edit targeted at one scope's mcp.json must NOT touch
// the other's. This is the invariant behind the plugin picker's fix — a row
// carries the .agentty dir it was read from, and remove/toggle route there
// (edit_target(row) in the settings reducer), so toggling a PROJECT server
// leaves the USER config untouched and vice-versa.
void scope_edits_are_isolated(const fs::path& dir) {
    std::println("--- scope_edits_are_isolated ---");
    const fs::path proj = dir / "proj" / ".agentty" / "mcp.json";
    const fs::path user = dir / "user" / ".agentty" / "mcp.json";
    // Same server name in both scopes, different commands.
    check(plug::add_server(proj, {"date", "/project/date", {}}, false) == plug::EditResult::Ok,
          "seed project-scope server");
    check(plug::add_server(user, {"date", "/user/date", {}}, false) == plug::EditResult::Ok,
          "seed user-scope server");

    // Disable ONLY the project server (mirrors a picker toggle on a project row).
    check(plug::set_server_disabled(proj, "date", true) == plug::EditResult::Ok,
          "disable project server");
    check(plug::is_server_disabled(proj, "date"),  "project server now disabled");
    check(!plug::is_server_disabled(user, "date"), "USER server untouched by project edit");
    check(read_json(user)["mcpServers"]["date"]["command"] == "/user/date",
          "user command preserved across a project-scope edit");

    // Remove ONLY the user server (mirrors `d` on a user row).
    check(plug::remove_server(user, "date") == plug::EditResult::Ok, "remove user server");
    check(!read_json(user)["mcpServers"].contains("date"), "user entry gone");
    check(read_json(proj)["mcpServers"].contains("date"),
          "PROJECT entry survives a user-scope removal");
    std::println("PASS\n");
}

int main() {
    const fs::path sandbox =
        fs::temp_directory_path() / ("agentty_plugin_test_" +
                                     std::to_string(::getpid()));
    fs::create_directories(sandbox);

    std::println("=== plugin_config_test ===");
    creates_file_on_first_add(sandbox);
    round_trip_preserves_foreign_keys(sandbox);
    honours_servers_spelling(sandbox);
    no_clobber_without_force(sandbox);
    distinct_not_found(sandbox);
    refuses_broken_json(sandbox);
    list_reads_back(sandbox);
    tool_enable_disable(sandbox);
    server_enable_disable(sandbox);
    scope_edits_are_isolated(sandbox);
    malformed_disabled_no_throw(sandbox);
    non_object_entry_no_throw(sandbox);
    concurrent_mutations_safe(sandbox);
    symlink_written_through(sandbox);

    std::error_code ec;
    fs::remove_all(sandbox, ec);

    if (g_failed) { std::println("{} check(s) FAILED", g_failed); return 1; }
    std::println("All plugin-config tests passed.");
    return 0;
}
