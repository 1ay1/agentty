// settings_list_view.cpp — the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list overlay, built on the house
// Picker widget so it frames/scrolls exactly like every other picker.
//
//   ┌─ Plugins ────────────────────────────────────┐
//   │  MCP servers · agentty plugin add …           │
//   │                                               │
//   │  ▎ today            python3 …/today.py  ✕     │
//   │    github           mcp-server-github   ✕     │
//   │                                               │
//   │  ↑↓ move · ↵ act · esc close                  │
//   └───────────────────────────────────────────────┘
//
// Rows come from settings::items_for(concern) live. Each row: name
// (leading, bold on selection), detail (trailing, dim), and a colour-coded
// action badge for the rows that DO something on Enter.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/settings_list.hpp"
#include "agentty/runtime/settings_items.hpp"

#include <maya/widget/picker.hpp>

#include <string>

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace se = agentty::settings;

namespace {

struct Badge { std::string glyph; Color color; };
Badge action_badge(se::Action a) {
    switch (a) {
        case se::Action::CycleProfile: return {"\xe2\x86\xbb", info};      // ↻
        case se::Action::OpenRag:
        case se::Action::OpenSmart:    return {"\xe2\x86\x92", highlight};  // →
        case se::Action::RemovePlugin: return {"\xe2\x9c\x95", danger};     // ✕
        case se::Action::ApproveHooks: return {"\xe2\x9c\x93", warn};       // ✓
        case se::Action::None:
        default:                       return {" ", muted};
    }
}

} // namespace

Element settings_list_picker(const Model& m) {
    const auto* o = settings_list_opened(m.ui.settings_list);
    if (!o) return nothing();

    auto rows = se::items_for(m, o->concern);

    Picker::Config cfg;
    cfg.title      = std::string{" "} + se::label(o->concern) + " ";
    cfg.accent     = highlight;   // cyan, matching the command palette
    cfg.min_width  = 64;
    cfg.viewport_h = 14;
    cfg.scroll     = nullptr;
    cfg.selected   = o->index;

    // Subtitle + separator as static header rows.
    cfg.header.push_back(h(
        text("  "), text(se::subtitle(o->concern), fg_dim(muted))
    ).build());
    cfg.header.push_back(text(""));

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& it = rows[static_cast<std::size_t>(i)];
        const Badge b = action_badge(it.action);

        Picker::Config::Row row;
        row.badge       = b.glyph;
        row.badge_style = fg_of(b.color);
        row.leading       = it.primary;
        row.leading_style = fg_of(fg);
        row.trailing       = it.secondary;
        row.trailing_style = fg_dim(muted);
        row.selected       = (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    // Footer: the action hint for the focused row (so "what does Enter do
    // here" is always answered), then the keybindings.
    std::string act_hint;
    if (o->index >= 0 && o->index < static_cast<int>(rows.size()))
        act_hint = rows[static_cast<std::size_t>(o->index)].hint;

    cfg.footer.push_back(text(""));
    if (!act_hint.empty())
        cfg.footer.push_back(h(
            text("  \xe2\x86\xb5 ", fg_of(highlight)),
            text(act_hint, fg_of(fg))
        ).build());
    cfg.footer.push_back(h(
        text("  \xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)), text("move   ", fg_dim(muted)),
        text("\xe2\x86\xb5 ", fg_of(fg)), text("act   ", fg_dim(muted)),
        text("esc ", fg_of(fg)), text("close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
