// plugin.cpp — the MCP plugin manager. See plugin.hpp for the contract.
// The editing functions are pure JSON-file surgery (testable without a
// terminal); cli() is the argv shell over them.

#include "agentty/tool/plugin.hpp"

#include "agentty/scope/scope.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>   // getpid
#else
#include <process.h>  // _getpid
#define getpid _getpid
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace agentty::tools::plugin {

namespace {

// All config MUTATIONS (add/remove/toggle) serialize behind this mutex. Each
// mutator is a load→modify→store cycle with no cross-process CAS; the reducer
// fires them on DETACHED worker threads (a "disable tool + disable server"
// pair, or rapid toggles), so without serialization two loads race and the
// second store clobbers the first's edit (classic lost update). One in-process
// mutex makes every load+store atomic w.r.t. other agentty mutations. (A
// concurrent EXTERNAL editor is still handled by the unique-temp + atomic
// rename in store(), so at worst one side's write wins wholesale — never a
// torn file.)
[[nodiscard]] std::mutex& mutation_mutex() {
    static std::mutex m;
    return m;
}

// Read an entry's `disabled` flag WITHOUT throwing on a malformed value. A
// hand-edited `"disabled": "true"` (string, not bool) makes nlohmann's
// value("disabled", false) throw type_error.302 — which would crash a toggle
// or the connect loop. Treat any non-bool as "not disabled" (fail-open: a
// garbled flag never silently hides a server).
[[nodiscard]] bool entry_disabled(const json& entry) noexcept {
    if (!entry.is_object()) return false;
    auto it = entry.find("disabled");
    return it != entry.end() && it->is_boolean() && it->get<bool>();
}

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
    // If the target is a SYMLINK, write through to its real target rather than
    // replacing the link with a regular file (a plain rename over a symlink
    // orphans a user's dotfile symlink). weakly_canonical resolves the link;
    // if it doesn't exist yet, we keep the original path.
    fs::path target = path;
    if (fs::is_symlink(path, ec)) {
        auto real = fs::weakly_canonical(path, ec);
        if (!ec && !real.empty()) target = real;
    }
    // Write-then-rename for atomicity: a crash mid-write must not leave a
    // truncated mcp.json (which would then hit the ParseError refusal on
    // every subsequent command — a self-inflicted lockout). The temp name is
    // UNIQUE (pid + a monotonic counter) so two concurrent writers — our own
    // detached mutators, or an external tool — never open, truncate, and
    // rename the SAME temp file (which would interleave into corruption).
    static std::atomic<unsigned long> seq{0};
    const fs::path tmp = target.string() + ".tmp."
        + std::to_string(static_cast<long>(::getpid())) + "-"
        + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << doc.dump(2) << '\n';
        f.flush();
        if (!f) { std::error_code rec; fs::remove(tmp, rec); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) { std::error_code rec; fs::remove(tmp, rec); return false; }
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

namespace {
// Must match the bridge's kMcpApprovalsLeaf + hashing exactly — both sides
// read/write the SAME user-root store keyed by the SAME content hash.
constexpr char kMcpApprovalsLeaf[] = "mcp_approvals.json";

// The current project mcp.json's content hash, or empty if there's no file.
[[nodiscard]] std::string project_config_hash() {
    const fs::path cfg = config_path(/*project=*/true);
    std::ifstream in(cfg, std::ios::binary);
    if (!in) return {};
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    return scope::content_hash(bytes);
}
}  // namespace

bool is_project_config_trusted() {
    if (const char* e = std::getenv("AGENTTY_MCP_ALLOW_PROJECT");
        e && (e[0] == '1' || e[0] == 't' || e[0] == 'T'
           || e[0] == 'y' || e[0] == 'Y'))
        return true;
    const std::string h = project_config_hash();
    if (h.empty()) return false;
    return scope::load_approvals(kMcpApprovalsLeaf).approved(h);
}

bool approve_project_config() {
    const std::string h = project_config_hash();
    if (h.empty()) return false;   // nothing to approve
    scope::Approvals a = scope::load_approvals(kMcpApprovalsLeaf);
    a.approve(h);
    return scope::save_approvals(kMcpApprovalsLeaf, a);
}

EditResult add_server(const fs::path& path, const ServerSpec& spec,
                      bool force) {
    std::lock_guard<std::mutex> lk(mutation_mutex());
    Loaded l = load(path);
    if (!l.ok) return EditResult::ParseError;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object())
        l.doc[key] = json::object();
    auto& servers = l.doc[key];
    if (servers.contains(spec.name) && !force)
        return EditResult::AlreadyExists;
    // `type: stdio` is the emerging cross-client convention (MCP config
    // standard proposals; Claude Code / VS Code / Cursor tag it). agentty's
    // add flow is command-based, so it's always stdio; writing it explicitly
    // makes the file portable to stricter clients that require the tag.
    json entry = {{"type", "stdio"}, {"command", spec.command}};
    if (!spec.args.empty()) entry["args"] = spec.args;
    // Overwrite-in-place (force) keeps any extra keys the user added to
    // THIS entry only when the command is unchanged in spirit — simplest
    // correct rule: force replaces the entry wholesale (documented).
    servers[spec.name] = std::move(entry);
    return store(path, l.doc) ? EditResult::Ok : EditResult::IoError;
}

EditResult remove_server(const fs::path& path, const std::string& name) {
    std::lock_guard<std::mutex> lk(mutation_mutex());
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

EditResult set_server_disabled(const fs::path& path, const std::string& name,
                              bool disabled) {
    std::lock_guard<std::mutex> lk(mutation_mutex());
    Loaded l = load(path);
    if (!l.existed) return EditResult::NotFound;
    if (!l.ok) return EditResult::ParseError;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()
        || !l.doc[key].contains(name))
        return EditResult::NotFound;
    json& entry = l.doc[key][name];
    if (!entry.is_object()) return EditResult::NotFound;
    const bool cur = entry_disabled(entry);
    if (cur == disabled) return EditResult::Ok;   // no-op-Ok
    if (disabled) entry["disabled"] = true;
    else          entry.erase("disabled");        // absent == enabled (clean)
    return store(path, l.doc) ? EditResult::Ok : EditResult::IoError;
}

bool is_server_disabled(const fs::path& path, const std::string& name) {
    Loaded l = load(path);
    if (!l.existed || !l.ok) return false;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()
        || !l.doc[key].contains(name))
        return false;
    const json& entry = l.doc[key][name];
    return entry_disabled(entry);
}

EditResult set_tool_enabled(const fs::path& path, const std::string& server,
                            const std::string& bare, bool enabled) {
    std::lock_guard<std::mutex> lk(mutation_mutex());
    Loaded l = load(path);
    if (!l.existed) return EditResult::NotFound;
    if (!l.ok) return EditResult::ParseError;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()
        || !l.doc[key].contains(server))
        return EditResult::NotFound;
    json& entry = l.doc[key][server];
    if (!entry.is_object()) return EditResult::NotFound;

    // tools.exclude is the disabled set. Enabling removes from it;
    // disabling adds. Preserve any tools.include/pin the user set.
    json& tools = entry["tools"];
    if (!tools.is_object()) tools = json::object();
    json& excl = tools["exclude"];
    if (!excl.is_array()) excl = json::array();

    // Rebuild the array either with or without `bare`.
    json rebuilt = json::array();
    bool present = false;
    for (const auto& v : excl) {
        if (v.is_string() && v.get<std::string>() == bare) { present = true; continue; }
        rebuilt.push_back(v);
    }
    if (!enabled) rebuilt.push_back(bare);   // disable = ensure present
    // No-op if already in the desired state.
    const bool changed = enabled ? present : !present;
    if (!changed) return EditResult::Ok;

    excl = std::move(rebuilt);
    if (excl.empty()) tools.erase("exclude");
    if (tools.empty()) entry.erase("tools");
    return store(path, l.doc) ? EditResult::Ok : EditResult::IoError;
}

bool is_tool_disabled(const fs::path& path, const std::string& server,
                      const std::string& bare) {
    for (const auto& d : disabled_tools(path, server))
        if (d == bare) return true;
    return false;
}

std::vector<std::string> disabled_tools(const fs::path& path,
                                        const std::string& server) {
    std::vector<std::string> out;
    Loaded l = load(path);
    if (!l.ok || !l.existed) return out;
    const char* key = servers_key(l.doc);
    if (!l.doc.contains(key) || !l.doc[key].is_object()
        || !l.doc[key].contains(server))
        return out;
    const json& entry = l.doc[key][server];
    if (!entry.is_object() || !entry.contains("tools")) return out;
    const json& tools = entry["tools"];
    if (!tools.is_object() || !tools.contains("exclude")) return out;
    const json& excl = tools["exclude"];
    if (!excl.is_array()) return out;
    for (const auto& v : excl)
        if (v.is_string()) out.push_back(v.get<std::string>());
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
