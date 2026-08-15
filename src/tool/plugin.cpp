// plugin.cpp — the MCP plugin manager. See plugin.hpp for the contract.
// The editing functions are pure JSON-file surgery (testable without a
// terminal); cli() is the argv shell over them.

#include "agentty/tool/plugin.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace agentty::tools::plugin {

namespace {

[[nodiscard]] fs::path home_dir() {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
    return {};
}

// Read + parse the file. Distinguishes "absent" (fresh empty doc, ok=true)
// from "present but broken" (ok=false — never rewrite a file we couldn't
// parse; a typo'd hand-edit must not be destroyed by `plugin add`).
struct Loaded {
    json doc = json::object();
    bool ok  = true;
    bool existed = false;
};

[[nodiscard]] Loaded load(const fs::path& path) {
    Loaded out;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return out;
    out.existed = true;
    std::ifstream f(path, std::ios::binary);
    if (!f) { out.ok = false; return out; }
    out.doc = json::parse(f, nullptr, /*throw=*/false);
    if (!out.doc.is_object()) { out.ok = false; out.doc = json::object(); }
    return out;
}

[[nodiscard]] bool store(const fs::path& path, const json& doc) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Write-then-rename for atomicity: a crash mid-write must not leave a
    // truncated mcp.json (which would then hit the ParseError refusal on
    // every subsequent command — a self-inflicted lockout).
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << doc.dump(2) << '\n';
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// The servers object key: honour an existing "servers" spelling (the
// bridge accepts both), default to the Claude-Desktop-compatible
// "mcpServers" for new files.
[[nodiscard]] const char* servers_key(const json& doc) {
    if (doc.contains("servers") && doc["servers"].is_object()
        && !doc.contains("mcpServers"))
        return "servers";
    return "mcpServers";
}

} // namespace

fs::path config_path(bool project) {
    if (project) return fs::path{".agentty"} / "mcp.json";
    auto h = home_dir();
    return (h.empty() ? fs::path{".agentty"} : h / ".agentty") / "mcp.json";
}

EditResult add_server(const fs::path& path, const ServerSpec& spec,
                      bool force) {
    Loaded l = load(path);
    if (!l.ok) return EditResult::ParseError;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object())
        l.doc[key] = json::object();
    auto& servers = l.doc[key];
    if (servers.contains(spec.name) && !force)
        return EditResult::AlreadyExists;
    json entry = {{"command", spec.command}};
    if (!spec.args.empty()) entry["args"] = spec.args;
    // Overwrite-in-place (force) keeps any extra keys the user added to
    // THIS entry only when the command is unchanged in spirit — simplest
    // correct rule: force replaces the entry wholesale (documented).
    servers[spec.name] = std::move(entry);
    return store(path, l.doc) ? EditResult::Ok : EditResult::IoError;
}

EditResult remove_server(const fs::path& path, const std::string& name) {
    Loaded l = load(path);
    if (!l.existed) return EditResult::NotFound;
    if (!l.ok) return EditResult::ParseError;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()
        || !l.doc[key].contains(name))
        return EditResult::NotFound;
    l.doc[key].erase(name);
    return store(path, l.doc) ? EditResult::Ok : EditResult::IoError;
}

std::vector<ServerSpec> list_servers(const fs::path& path) {
    std::vector<ServerSpec> out;
    Loaded l = load(path);
    if (!l.ok || !l.existed) return out;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()) return out;
    for (auto& [name, entry] : l.doc[key].items()) {
        ServerSpec s;
        s.name = name;
        if (entry.is_object()) {
            s.command = entry.value("command", std::string{});
            if (entry.contains("args") && entry["args"].is_array())
                for (const auto& a : entry["args"])
                    if (a.is_string()) s.args.push_back(a.get<std::string>());
        }
        out.push_back(std::move(s));
    }
    return out;
}

namespace {

int usage() {
    std::fprintf(stderr,
        "usage: agentty plugin <verb> …   (a plugin IS an MCP server entry)\n"
        "\n"
        "  add <name> --uvx <pypi-pkg> [extra args…]\n"
        "        Python plugin from PyPI, run via uv (auto-installs,\n"
        "        isolated env): command = uvx <pkg> …\n"
        "  add <name> --python <script.py> [extra args…]\n"
        "        Local Python script: command = python3 <script> …\n"
        "  add <name> --npx <npm-pkg> [extra args…]\n"
        "        Node plugin from npm: command = npx -y <pkg> …\n"
        "  add <name> -- <command> [args…]\n"
        "        Anything else, verbatim argv.\n"
        "\n"
        "  options for add: --project (write ./.agentty/mcp.json instead of\n"
        "        ~/.agentty/mcp.json), --force (overwrite an existing name)\n"
        "\n"
        "  list  [--project]      show configured plugins\n"
        "  remove <name> [--project]\n"
        "\n"
        "Connected at startup; restart agentty (or start a new session) to\n"
        "pick up changes. Project-scope configs additionally require\n"
        "AGENTTY_MCP_ALLOW_PROJECT=1 before agentty connects to them.\n");
    return 2;
}

} // namespace

int cli(const std::vector<std::string>& argv) {
    if (argv.empty()) return usage();
    const std::string& verb = argv[0];

    // Common flags, position-independent after the verb.
    bool project = false, force = false;
    std::vector<std::string> rest;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        if      (argv[i] == "--project") project = true;
        else if (argv[i] == "--force")   force = true;
        else rest.push_back(argv[i]);
    }
    const fs::path path = config_path(project);

    if (verb == "list") {
        auto servers = list_servers(path);
        if (servers.empty()) {
            std::printf("no plugins in %s\n", path.string().c_str());
            return 0;
        }
        std::printf("%s:\n", path.string().c_str());
        for (const auto& s : servers) {
            std::string cmdline = s.command;
            for (const auto& a : s.args) cmdline += " " + a;
            std::printf("  %-16s %s\n", s.name.c_str(), cmdline.c_str());
        }
        return 0;
    }

    if (verb == "remove") {
        if (rest.size() != 1) return usage();
        switch (remove_server(path, rest[0])) {
        case EditResult::Ok:
            std::printf("removed %s from %s\n", rest[0].c_str(),
                        path.string().c_str());
            return 0;
        case EditResult::NotFound:
            std::fprintf(stderr, "no plugin named '%s' in %s\n",
                         rest[0].c_str(), path.string().c_str());
            return 1;
        case EditResult::ParseError:
            std::fprintf(stderr, "%s is not valid JSON — fix it by hand "
                         "first (refusing to rewrite a broken file)\n",
                         path.string().c_str());
            return 1;
        default:
            std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            return 1;
        }
    }

    if (verb == "add") {
        // Shape: <name> then exactly one recipe flag (or `--`).
        if (rest.size() < 2) return usage();
        ServerSpec spec;
        spec.name = rest[0];
        const std::string& recipe = rest[1];
        std::vector<std::string> tail(rest.begin() + 2, rest.end());

        if (recipe == "--uvx") {
            if (tail.empty()) return usage();
            spec.command = "uvx";
            spec.args = std::move(tail);
        } else if (recipe == "--python") {
            if (tail.empty()) return usage();
            spec.command = "python3";
            // Absolutise the script path: agentty's cwd at connect time is
            // whatever directory the user launches from, not where they ran
            // `plugin add` — a relative path would break silently.
            std::error_code ec;
            fs::path script = fs::absolute(tail[0], ec);
            if (!ec) tail[0] = script.string();
            spec.args = std::move(tail);
        } else if (recipe == "--npx") {
            if (tail.empty()) return usage();
            spec.command = "npx";
            spec.args.push_back("-y");
            for (auto& t : tail) spec.args.push_back(std::move(t));
        } else if (recipe == "--") {
            if (tail.empty()) return usage();
            spec.command = tail[0];
            spec.args.assign(tail.begin() + 1, tail.end());
        } else {
            return usage();
        }

        switch (add_server(path, spec, force)) {
        case EditResult::Ok: {
            std::string cmdline = spec.command;
            for (const auto& a : spec.args) cmdline += " " + a;
            std::printf("added %-16s %s\n  → %s\n", spec.name.c_str(),
                        cmdline.c_str(), path.string().c_str());
            std::printf("restart agentty to connect (tools appear as "
                        "mcp__%s__<tool>)\n", spec.name.c_str());
            if (project)
                std::printf("note: project configs need "
                            "AGENTTY_MCP_ALLOW_PROJECT=1 to connect\n");
            return 0;
        }
        case EditResult::AlreadyExists:
            std::fprintf(stderr, "'%s' already exists in %s "
                         "(use --force to overwrite)\n",
                         spec.name.c_str(), path.string().c_str());
            return 1;
        case EditResult::ParseError:
            std::fprintf(stderr, "%s is not valid JSON — fix it by hand "
                         "first (refusing to rewrite a broken file)\n",
                         path.string().c_str());
            return 1;
        default:
            std::fprintf(stderr, "could not write %s\n", path.string().c_str());
            return 1;
        }
    }

    return usage();
}

} // namespace agentty::tools::plugin
