// settings_items.cpp — build each category's rows live from the loaders.

#include "agentty/runtime/settings_items.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/tool/hooks.hpp"

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include "agentty/mcp/client.hpp"   // PluginModel, plugin_model()
#include <sstream>

namespace fs = std::filesystem;

namespace agentty::settings {

namespace {

std::string plugins_config_display() {
    // Prefer whichever config actually exists, mirroring the manager's
    // default (user scope).
    auto user = tools::plugin::config_path(/*project=*/false);
    return user.string();
}

std::vector<Item> general(const Model& m) {
    std::vector<Item> out;

    // Permission profile.
    {
        Item i;
        i.primary   = "Permission profile";
        i.secondary = std::string("current: ") +
                      std::string(to_string(m.d.profile));
        i.hint      = "Enter: cycle";
        i.action    = Action::CycleProfile;
        out.push_back(std::move(i));
    }
    // Smart Mode.
    {
        Item i;
        i.primary   = "Smart Mode";
        i.secondary = m.d.smart.enabled ? "on — role-based routing active"
                                        : "off";
        i.hint      = "Enter: configure";
        i.action    = Action::OpenSmart;
        out.push_back(std::move(i));
    }
    // RAG proactive retrieval.
    {
        Item i;
        i.primary   = "Proactive retrieval (RAG)";
        i.secondary = "pre-turn context injection";
        i.hint      = "Enter: configure";
        i.action    = Action::OpenRag;
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<Item> plugins() {
    // Pure projection of the authoritative PluginModel snapshot. No
    // reconciliation of live-catalog vs config here — the model already
    // unified them, so a tool can never vanish or duplicate.
    std::vector<Item> out;
    const agentty::mcp::PluginModel model = agentty::mcp::plugin_model();

    if (model.servers.empty()) {
        Item i;
        i.primary   = "(no plugins configured)";
        i.secondary = "press `a` to add one, or agentty plugin add <name> …";
        i.hint      = "docs/PLUGINS.md";
        out.push_back(std::move(i));
        return out;
    }

    // Header: total tools on the wire + budget warning.
    {
        Item w;
        if (model.over_budget()) {
            w.primary   = "⚠  " + std::to_string(model.wire_tool_count) +
                          " tools (budget " + std::to_string(model.tool_budget) + ")";
            w.secondary = std::to_string(model.trimmed_count) +
                          " over budget were dropped from this session — "
                          "disable some below to make room";
        } else {
            w.primary   = std::to_string(model.wire_tool_count) +
                          " tools on the wire";
            w.secondary = "◉ on / ○ off · Enter toggles a tool or removes a plugin";
        }
        out.push_back(std::move(w));
    }

    for (const auto& s : model.servers) {
        Item i;
        i.primary   = s.name;
        if (!s.error.empty()) {
            i.secondary = "⚠ " + s.error;
        } else if (!s.connected) {
            i.secondary = "connecting…";
        } else {
            i.secondary = std::to_string(s.enabled_count()) + "/" +
                          std::to_string(s.tools.size()) + " tools";
        }
        i.hint      = "remove";
        i.action    = Action::RemovePlugin;
        i.arg       = s.name;
        out.push_back(std::move(i));

        for (const auto& t : s.tools) {
            Item ti;
            ti.primary   = t.name;
            ti.secondary = t.over_budget ? "over budget — not on the wire" : "";
            ti.hint      = t.enabled ? "disable" : "enable";
            ti.action    = Action::ToggleTool;
            ti.arg       = s.name;
            ti.arg2      = t.name;
            ti.indented  = true;
            ti.on        = t.enabled;
            out.push_back(std::move(ti));
        }
    }
    return out;
}

std::vector<Item> commands() {
    std::vector<Item> out;
    for (const auto& c : tools::commands::all()) {
        Item i;
        i.primary   = "/" + c.name;
        i.secondary = c.description;
        i.hint      = c.source;   // project | user
        out.push_back(std::move(i));
    }
    if (out.empty()) {
        Item i;
        i.primary   = "(no slash commands)";
        i.secondary = "author one: .agentty/commands/<name>.md";
        i.hint      = "docs/PLUGINS.md";
        out.push_back(std::move(i));
    }
    return out;
}

std::vector<Item> agents() {
    std::vector<Item> out;
    // Built-ins first (always available), then user agents.
    for (const char* b : {"explorer", "reviewer", "tester", "coder", "general"}) {
        Item i;
        i.primary   = b;
        i.secondary = "built-in";
        out.push_back(std::move(i));
    }
    // User agents are discovered by the task backend; surface the authoring
    // path so the pane is self-documenting even before any exist.
    Item hint;
    hint.primary   = "+ user agents";
    hint.secondary = "author one: .agentty/agents/<name>.md "
                     "(frontmatter: tools, read-only)";
    hint.hint      = "docs/PLUGINS.md";
    out.push_back(std::move(hint));
    return out;
}

std::vector<Item> hooks() {
    std::vector<Item> out;
    const std::string file = tools::hooks::active_file();
    if (file.empty()) {
        Item i;
        i.primary   = "(no hooks file)";
        i.secondary = "author one: .agentty/hooks.json "
                      "(pre_tool / post_tool)";
        i.hint      = "docs/PLUGINS.md";
        out.push_back(std::move(i));
        return out;
    }
    Item i;
    i.primary = file;
    if (tools::hooks::pending_approval()) {
        i.secondary = "NOT APPROVED — hooks will not run";
        i.hint      = "Enter: review & approve";
        i.action    = Action::ApproveHooks;
    } else {
        i.secondary = "approved — active";
        i.hint      = "";
    }
    out.push_back(std::move(i));
    return out;
}

} // namespace

std::vector<Item> items_for(const Model& m, Category cat) {
    switch (cat) {
        case Category::General:  return general(m);
        case Category::Plugins:  return plugins();
        case Category::Commands: return commands();
        case Category::Agents:   return agents();
        case Category::Hooks:    return hooks();
    }
    return {};
}

namespace {

fs::path home_dir_() {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
    return {};
}

// Whitespace-split a line into tokens.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(std::move(tok));
    return out;
}

// A slug safe for a filename: keep [A-Za-z0-9_:-], collapse the rest.
bool valid_name(const std::string& n) {
    if (n.empty()) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'
              || c == '-' || c == ':'))
            return false;
    return true;
}

} // namespace

AddResult add_plugin_from_line(const std::string& line) {
    auto tok = split_ws(line);
    if (tok.size() < 2)
        return {false, "usage: <name> <command> [args…]  — or  "
                       "<name> --python file.py / --uvx pkg / --npx pkg"};
    const std::string name = tok[0];
    if (!valid_name(name))
        return {false, "plugin name may use letters, digits, _ - : only"};

    tools::plugin::ServerSpec spec;
    spec.name = name;
    const std::string& recipe = tok[1];
    std::vector<std::string> rest(tok.begin() + 2, tok.end());

    if (recipe == "--python") {
        if (rest.empty()) return {false, "--python needs a script path"};
        spec.command = "python3";
        std::error_code ec;
        fs::path abs = fs::absolute(rest[0], ec);
        if (!ec) rest[0] = abs.string();
        spec.args = std::move(rest);
    } else if (recipe == "--uvx") {
        if (rest.empty()) return {false, "--uvx needs a package name"};
        spec.command = "uvx";
        spec.args = std::move(rest);
    } else if (recipe == "--npx") {
        if (rest.empty()) return {false, "--npx needs a package name"};
        spec.command = "npx";
        spec.args.push_back("-y");
        for (auto& t : rest) spec.args.push_back(std::move(t));
    } else {
        spec.command = recipe;
        spec.args = std::move(rest);
    }

    const auto path = tools::plugin::config_path(/*project=*/false);
    switch (tools::plugin::add_server(path, spec, /*force=*/false)) {
        case tools::plugin::EditResult::Ok:
            return {true, "added plugin '" + name + "'"};
        case tools::plugin::EditResult::AlreadyExists:
            return {false, "'" + name + "' already exists"};
        case tools::plugin::EditResult::ParseError:
            return {false, "mcp.json is not valid JSON — fix it by hand"};
        default:
            return {false, "could not write mcp.json"};
    }
}

AddResult create_starter(Category cat, const std::string& name) {
    if (!valid_name(name))
        return {false, "name may use letters, digits, _ - : only"};
    const fs::path home = home_dir_();
    if (home.empty()) return {false, "no HOME to write under"};

    const char* sub = nullptr;
    std::string tmpl;
    if (cat == Category::Commands) {
        sub = "commands";
        tmpl = "---\ndescription: " + name + " command\n"
               "argument-hint: <args>\n---\n"
               "Do the thing for $ARGUMENTS.\n";
    } else if (cat == Category::Agents) {
        sub = "agents";
        tmpl = "---\ndescription: " + name + " agent\n"
               "read-only: false\n"
               "# tools: read grep glob list_dir   # optional allowlist\n"
               "---\nYour role: " + name +
               ". Complete the delegated task end-to-end, then report.\n";
    } else {
        return {false, "create_starter only supports commands/agents"};
    }

    const fs::path dir = home / ".agentty" / sub;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (name + ".md");
    if (fs::exists(file, ec))
        return {false, "already exists: " + file.string()};
    std::ofstream f(file, std::ios::binary);
    if (!f) return {false, "could not create " + file.string()};
    f << tmpl;
    if (!f) return {false, "write failed: " + file.string()};

    // Force a rescan so the new entry shows on the next open.
    if (cat == Category::Commands) tools::commands::invalidate_cache();
    return {true, "created " + file.string() + " — edit it, then reopen"};
}

} // namespace agentty::settings
