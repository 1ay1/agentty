// mcp/import.cpp — adopt another tool's MCP servers. See import.hpp for why
// this is an import and not a sixth read root.

#include "agentty/mcp/import.hpp"

#include "agentty/config/inventory.hpp"
#include "agentty/scope/scope.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/util/home_dir.hpp"
#include "agentty/util/logx.hpp"
#include "agentty/util/user_root.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <set>

namespace agentty::mcp::import_ {

using json = nlohmann::json;

namespace {

// Pull an `mcpServers` / `servers` object out of a document. Both spellings
// are in the wild (agentty accepts either too), so neither is "wrong".
[[nodiscard]] const json* servers_of(const json& doc) noexcept {
    if (!doc.is_object()) return nullptr;
    if (auto it = doc.find("mcpServers"); it != doc.end() && it->is_object())
        return &*it;
    if (auto it = doc.find("servers"); it != doc.end() && it->is_object())
        return &*it;
    return nullptr;
}

// Translate one foreign entry into our shape.
//
// The fields we understand are the ones every tool agrees on, because they
// come from the MCP spec itself: a stdio server is command+args, a remote one
// is a url. Anything else (env blocks, headers, timeouts) is deliberately NOT
// carried: a half-translated secret is worse than an obvious gap, and the
// import prints what it took so the user can tell.
[[nodiscard]] bool translate(const std::string& name, const json& e,
                             const fs::path& from, Found& out) {
    if (!e.is_object()) return false;
    out.name = name;
    out.from = from;
    if (auto it = e.find("command"); it != e.end() && it->is_string())
        out.command = it->get<std::string>();
    if (auto it = e.find("url"); it != e.end() && it->is_string())
        out.url = it->get<std::string>();
    if (auto it = e.find("type"); it != e.end() && it->is_string())
        out.type = it->get<std::string>();
    if (auto it = e.find("args"); it != e.end() && it->is_array())
        for (const auto& a : *it)
            if (a.is_string()) out.args.push_back(a.get<std::string>());
    // A server with neither is not a server. Skipping beats importing a
    // broken entry that then fails to connect with no explanation.
    return !out.command.empty() || !out.url.empty();
}

// Claude Code keeps per-project servers under projects.<abs-path>.mcpServers
// in one big ~/.claude.json. Reach into the entry for THIS project only --
// adopting another checkout's servers would be a surprise.
void scan_claude_nested(const json& doc, const fs::path& project,
                        const fs::path& from, std::vector<Found>& out) {
    auto projects = doc.find("projects");
    if (projects == doc.end() || !projects->is_object()) return;
    const std::string key = project.string();
    auto mine = projects->find(key);
    if (mine == projects->end() || !mine->is_object()) return;
    const json* servers = servers_of(*mine);
    if (!servers) return;
    for (auto it = servers->begin(); it != servers->end(); ++it) {
        Found f;
        if (translate(it.key(), it.value(), from, f)) out.push_back(std::move(f));
    }
}

void scan_file(const fs::path& file, const fs::path& project,
               std::vector<Found>& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return;
    std::ifstream in(file);
    if (!in) return;
    json doc = json::parse(in, nullptr, /*throw=*/false);
    if (doc.is_discarded()) {
        // Refuse rather than guess. A malformed foreign config is the user's
        // to fix; importing half of it would be worse than importing none.
        error = "could not parse " + file.string();
        return;
    }
    if (const json* servers = servers_of(doc)) {
        for (auto it = servers->begin(); it != servers->end(); ++it) {
            Found f;
            if (translate(it.key(), it.value(), file, f))
                out.push_back(std::move(f));
        }
    }
    scan_claude_nested(doc, project, file, out);
}

// The names agentty already has, so a scan can flag collisions without the
// caller re-reading our own config.
[[nodiscard]] std::set<std::string> our_server_names() {
    std::set<std::string> names;
    const scope::Env env = scope::current_env(config::kMcpLayout);
    for (const scope::Source& src : scope::plan(config::kMcpLayout, env)) {
        const fs::path file = (src.locus == scope::Locus::Explicit
                               && env.explicit_config)
            ? *env.explicit_config
            : src.base / fs::path{config::kMcpLayout.leaf};
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        std::ifstream in(file);
        json doc = json::parse(in, nullptr, false);
        if (const json* s = servers_of(doc))
            for (auto it = s->begin(); it != s->end(); ++it)
                names.insert(it.key());
    }
    return names;
}

}  // namespace

std::string_view name_of(Tool t) noexcept {
    switch (t) {
        case Tool::Junie:      return "junie";
        case Tool::ClaudeCode: return "claude";
        case Tool::Cursor:     return "cursor";
        case Tool::VsCode:     return "vscode";
    }
    return "?";
}

std::vector<Tool> all_tools() {
    return {Tool::Junie, Tool::ClaudeCode, Tool::Cursor, Tool::VsCode};
}

bool parse_tool(std::string_view s, Tool& out) noexcept {
    for (Tool t : {Tool::Junie, Tool::ClaudeCode, Tool::Cursor, Tool::VsCode})
        if (name_of(t) == s) { out = t; return true; }
    // Accept the obvious aliases rather than being pedantic about it.
    if (s == "jetbrains" || s == "intellij") { out = Tool::Junie; return true; }
    if (s == "claude-code")                  { out = Tool::ClaudeCode; return true; }
    if (s == "code")                         { out = Tool::VsCode; return true; }
    return false;
}

Locations locations_of(Tool t, const fs::path& home, const fs::path& project) {
    switch (t) {
        case Tool::Junie:
            return {home / ".junie" / "mcp" / "mcp.json",
                    project / ".junie" / "mcp" / "mcp.json"};
        case Tool::ClaudeCode:
            // User scope is one big file with servers nested per project;
            // project scope is the portable .mcp.json at the repo root.
            return {home / ".claude.json", project / ".mcp.json"};
        case Tool::Cursor:
            return {home / ".cursor" / "mcp.json",
                    project / ".cursor" / "mcp.json"};
        case Tool::VsCode:
            // No documented user-level file; workspace only.
            return {{}, project / ".vscode" / "mcp.json"};
    }
    return {};
}

Scan scan(Tool t, const fs::path& home, const fs::path& project) {
    Scan s;
    s.tool = t;
    const Locations loc = locations_of(t, home, project);
    const auto existing = our_server_names();
    for (const fs::path& p : {loc.user, loc.project}) {
        if (p.empty()) continue;
        s.looked_in.push_back(p);
        scan_file(p, project, s.servers, s.error);
    }
    for (Found& f : s.servers) f.conflicts = existing.contains(f.name);
    AGT_LOG(Persist, Debug, "mcp", "import.scan {}: {} server(s) in {} path(s)",
            name_of(t), s.servers.size(), s.looked_in.size());
    return s;
}

std::vector<Scan> scan_all(const fs::path& home, const fs::path& project) {
    std::vector<Scan> out;
    for (Tool t : all_tools()) {
        Scan s = scan(t, home, project);
        if (!s.servers.empty() || !s.error.empty()) out.push_back(std::move(s));
    }
    return out;
}

Outcome adopt(const std::vector<Found>& servers, const Options& opts) {
    Outcome o;
    o.into = util::user_root() / "mcp.json";
    for (const Found& f : servers) {
        if (f.conflicts && !opts.force) { ++o.skipped; continue; }
        if (opts.dry_run) { ++o.imported; continue; }
        tools::plugin::ServerSpec spec;
        spec.name    = f.name;
        spec.command = f.command;
        spec.args    = f.args;
        spec.url     = f.url;
        spec.type    = f.type;
        const auto r = tools::plugin::add_server(o.into, spec, opts.force);
        if (r == tools::plugin::EditResult::Ok) ++o.imported;
        else                                    ++o.failed;
    }
    AGT_LOG(Persist, Info, "mcp", "import.adopt: {} in, {} skipped, {} failed",
            o.imported, o.skipped, o.failed);
    return o;
}

namespace {

void print_scan(const Scan& s) {
    std::printf("%s\n", std::string{name_of(s.tool)}.c_str());
    for (const fs::path& p : s.looked_in) {
        std::error_code ec;
        std::printf("  %s%s\n", p.string().c_str(),
                    fs::is_regular_file(p, ec) ? "" : "   (missing)");
    }
    if (!s.error.empty()) {
        std::printf("  error: %s\n", s.error.c_str());
        return;
    }
    if (s.servers.empty()) { std::printf("  no servers\n"); return; }
    for (const Found& f : s.servers)
        std::printf("    %-20s %s%s\n", f.name.c_str(),
                    f.command.empty() ? f.url.c_str() : f.command.c_str(),
                    f.conflicts ? "   (agentty already has this name)" : "");
}

}  // namespace

int cli(const std::vector<std::string>& argv) {
    Options opts;
    std::string want;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "--dry-run" || a == "-n") opts.dry_run = true;
        else if (a == "--force")           opts.force   = true;
        else if (a == "--from" && i + 1 < argv.size()) want = argv[++i];
        else if (a.rfind("--from=", 0) == 0) want = a.substr(7);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: agentty mcp import [--from <tool>] [--dry-run] [--force]\n\n"
                "Copy another tool's MCP servers into ~/.agentty/mcp.json.\n"
                "Known tools: junie (jetbrains), claude, cursor, vscode.\n\n"
                "With no --from, lists what every known tool offers.\n"
                "A name agentty already has is left alone unless --force.\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    const fs::path home = util::home_dir_or_empty();
    std::error_code ec;
    const fs::path project = fs::current_path(ec);

    // No --from: report, adopt nothing. Discovery should never be a mutation
    // -- "what do you see?" and "take it" are different questions.
    if (want.empty()) {
        const auto scans = scan_all(home, project);
        if (scans.empty()) {
            std::printf("no MCP config found for any known tool.\n"
                        "looked for: junie, claude, cursor, vscode\n");
            return 0;
        }
        for (const Scan& s : scans) print_scan(s);
        std::printf("\n`agentty mcp import --from <tool>` to adopt.\n");
        return 0;
    }

    Tool t{};
    if (!parse_tool(want, t)) {
        std::fprintf(stderr, "unknown tool: %s\n\nknown:", want.c_str());
        for (Tool k : all_tools())
            std::fprintf(stderr, " %s", std::string{name_of(k)}.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }

    const Scan s = scan(t, home, project);
    print_scan(s);
    if (!s.error.empty()) return 1;
    if (s.servers.empty()) return 0;

    const Outcome o = adopt(s.servers, opts);
    std::printf("\n%s %d server(s) %s %s\n",
                opts.dry_run ? "would import" : "imported",
                o.imported,
                opts.dry_run ? "into" : "into",
                o.into.string().c_str());
    if (o.skipped > 0)
        std::printf("skipped %d name(s) agentty already has"
                    " (--force to replace)\n", o.skipped);
    if (o.failed > 0) {
        std::printf("%d failed to write\n", o.failed);
        return 1;
    }
    return 0;
}

}  // namespace agentty::mcp::import_
