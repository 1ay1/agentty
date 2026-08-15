#pragma once
// Settings pickers — the Ctrl+K rows for the config concerns that used to
// be CLI-/file-only: Plugins, Commands, Agents, Hooks. Each is a palette
// entry that opens THIS one picker, parameterised by `concern`. A single
// list modal (matching every other picker's shape) rather than a bespoke
// pane: pick the concern from Ctrl+K, get a focused list, act on a row.
//
// The rows are built live from settings::items_for(concern) — the same
// loaders the runtime uses — so the picker always mirrors disk. Actionable
// rows (remove a plugin, approve hooks, open RAG/Smart) act on Enter; the
// rest are informational. Keys (subscribe.cpp::on_settings_list): ↑↓/jk
// move, Enter acts, Esc/q closes.
//
// UI-state only; reducer in update/settings_list.cpp, view in
// view/settings_list_view.cpp.

#include <cstdint>
#include <variant>

#include "agentty/runtime/settings_categories.hpp"   // settings::Category

namespace agentty {

namespace settings {

struct ListClosed {};
struct ListOpen {
    Category concern = Category::Plugins;
    int      index   = 0;
};

} // namespace settings

using SettingsListState =
    std::variant<settings::ListClosed, settings::ListOpen>;

[[nodiscard]] inline bool
settings_list_is_open(const SettingsListState& s) noexcept {
    return std::holds_alternative<settings::ListOpen>(s);
}
[[nodiscard]] inline settings::ListOpen*
settings_list_opened(SettingsListState& s) noexcept {
    return std::get_if<settings::ListOpen>(&s);
}
[[nodiscard]] inline const settings::ListOpen*
settings_list_opened(const SettingsListState& s) noexcept {
    return std::get_if<settings::ListOpen>(&s);
}

} // namespace agentty
