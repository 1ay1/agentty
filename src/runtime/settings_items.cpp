// settings_items.cpp — build each category's rows live from the loaders.

#include "agentty/runtime/settings_items.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/tool/hooks.hpp"

#include <filesystem>

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
    std::vector<Item> out;
    const auto path = tools::plugin::config_path(false);
    auto servers = tools::plugin::list_servers(path);
    if (servers.empty()) {
        Item i;
        i.primary   = "(no plugins configured)";
        i.secondary = "add one: agentty plugin add <name> --python … / --uvx …";
        i.hint      = "docs/PLUGINS.md";
        out.push_back(std::move(i));
        return out;
    }
    for (const auto& s : servers) {
        Item i;
        i.primary = s.name;
        std::string cmd = s.command;
        for (const auto& a : s.args) cmd += " " + a;
        i.secondary = std::move(cmd);
        i.hint      = "Enter: remove";
        i.action    = Action::RemovePlugin;
        i.arg       = s.name;
        out.push_back(std::move(i));
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

} // namespace agentty::settings
