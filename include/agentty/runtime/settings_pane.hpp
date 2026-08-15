#pragma once
// Settings pane — the Ctrl+K → "Settings" hub for everything that used to
// be CLI-/file-only: plugins, slash commands, subagents, hooks, plus the
// live toggles (RAG mode, Smart Mode, permission profile).
//
// UX: a two-column overlay. LEFT is the category list; RIGHT is that
// category's items, rendered live from the same loaders the runtime uses
// (tools::plugin::list_servers, tools::commands::all, the user-agent
// scanner, tools::hooks). Read-only categories (Commands/Agents) show what
// is discovered and where; actionable ones (Plugins remove, Hooks approve,
// RAG/Smart/Profile) act on Enter.
//
// Keys (handled in subscribe.cpp::on_settings): ←/→ or Tab switch column,
// ↑↓/jk move, Enter acts on the focused item, Esc/q closes. Deeper edits
// (add a plugin, author a command) intentionally stay in the CLI/editor —
// the pane is for DISCOVERY + the safe one-key actions, not a JSON editor
// in a terminal. Each actionable row shows the CLI it maps to, so the pane
// teaches the command line rather than hiding it.
//
// UI-state only; reducer in update/settings.cpp, view in view/settings_view.cpp.

#include <cstdint>
#include <variant>

namespace agentty {

namespace settings {

enum class Category : std::uint8_t {
    General,   // profile, RAG mode, Smart Mode — the live toggles
    Plugins,   // MCP servers (mcp.json): list + remove
    Commands,  // slash commands: discovered list (read-only)
    Agents,    // user subagents: discovered list (read-only)
    Hooks,     // lifecycle hooks: file + approval state + approve action
};

inline constexpr Category kCategories[] = {
    Category::General, Category::Plugins, Category::Commands,
    Category::Agents,  Category::Hooks,
};
inline constexpr int kCategoryCount = 5;

[[nodiscard]] constexpr const char* label(Category c) noexcept {
    switch (c) {
        case Category::General:  return "General";
        case Category::Plugins:  return "Plugins";
        case Category::Commands: return "Commands";
        case Category::Agents:   return "Agents";
        case Category::Hooks:    return "Hooks";
    }
    return "?";
}

// Which column has keyboard focus.
enum class Focus : std::uint8_t { Categories, Items };

struct Closed {};
struct Open {
    Category cat        = Category::General;
    int      cat_index  = 0;   // cursor in the category column
    int      item_index = 0;   // cursor in the item column
    Focus    focus      = Focus::Categories;
};

} // namespace settings

using SettingsState = std::variant<settings::Closed, settings::Open>;

[[nodiscard]] inline bool settings_is_open(const SettingsState& s) noexcept {
    return std::holds_alternative<settings::Open>(s);
}
[[nodiscard]] inline settings::Open* settings_opened(SettingsState& s) noexcept {
    return std::get_if<settings::Open>(&s);
}
[[nodiscard]] inline const settings::Open*
settings_opened(const SettingsState& s) noexcept {
    return std::get_if<settings::Open>(&s);
}

} // namespace agentty
