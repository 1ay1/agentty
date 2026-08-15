// settings_items.hpp — the per-concern row model for the settings pickers
// (Ctrl+K → Plugins/Commands/Agents/Hooks). Built fresh each call from the
// live loaders so the picker always mirrors what's actually on disk.

#pragma once

#include <string>
#include <vector>

#include "agentty/runtime/settings_categories.hpp"

namespace agentty { struct Model; }

namespace agentty::settings {

// What activating a row does. Kept abstract so the reducer switches on the
// kind and the view renders the hint, without either hard-coding indices.
enum class Action : std::uint8_t {
    None,          // informational row (no Enter action)
    CycleProfile,  // General: Write → Ask → Minimal
    OpenRag,       // General: open the RAG mode picker
    OpenSmart,     // General: open Smart Mode config
    RemovePlugin,  // Plugins: remove this server from mcp.json
    ApproveHooks,  // Hooks: approve the active hooks file
};

struct Item {
    std::string primary;    // left/main text (name)
    std::string secondary;  // dim detail (command line, path, state)
    std::string hint;       // right-aligned action/CLI hint
    Action      action = Action::None;
    std::string arg;        // action payload (e.g. plugin name to remove)
};

// The rows for one category, live. `m` supplies profile/RAG/Smart state
// for the General category; the rest come from the tools:: loaders.
[[nodiscard]] std::vector<Item> items_for(const Model& m, Category cat);

} // namespace agentty::settings
