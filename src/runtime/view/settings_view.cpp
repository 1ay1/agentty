// settings_view.cpp — the Settings pane overlay (Ctrl+K → Settings).
//
// Rendered with the house Picker widget for a consistent frame/scroll:
//   • title  = the category tab strip, the focused/active category
//              bracketed — [General] Plugins Commands Agents Hooks
//   • rows   = that category's live items (name + detail + action hint)
//   • footer = focus indicator + keybindings
//
// The two-column focus model (categories vs items) is projected onto this
// single widget: when focus is on Categories the tab strip is bright and
// the rows are dimmed; when focus is on Items the rows carry the cursor.
// Everything is read fresh from settings::items_for so the pane always
// mirrors what's on disk.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/settings_pane.hpp"
#include "agentty/runtime/settings_items.hpp"

#include <maya/widget/picker.hpp>

#include <string>

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace se = agentty::settings;

namespace {

// The category tab strip for the border title. The active category is
// bracketed; when the categories column has focus it reads as "you are
// choosing a category", when items has focus the brackets still show
// context but the rows below own the cursor.
std::string tab_strip(const se::Open& o) {
    std::string s = " ";
    for (int i = 0; i < se::kCategoryCount; ++i) {
        const char* lbl = se::label(se::kCategories[i]);
        if (i == o.cat_index) { s += "["; s += lbl; s += "]"; }
        else                  { s += " "; s += lbl; s += " "; }
        s += " ";
    }
    return s;
}

} // namespace

Element settings_pane(const Model& m) {
    const auto* o = settings_opened(m.ui.settings);
    if (!o) return nothing();

    const bool items_focus = (o->focus == se::Focus::Items);
    auto rows = se::items_for(m, o->cat);

    Picker::Config cfg;
    cfg.title      = tab_strip(*o);
    cfg.accent     = highlight;   // cyan, matching the command palette
    cfg.min_width  = 66;
    cfg.viewport_h = 12;
    cfg.scroll     = nullptr;
    cfg.selected   = items_focus ? o->item_index : -1;

    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& it = rows[static_cast<std::size_t>(i)];
        Picker::Config::Row row;
        // Action rows get a subtle marker so "this does something on Enter"
        // is visible at a glance.
        row.badge       = (it.action != se::Action::None) ? "\xe2\x80\xa2" : " ";
        row.badge_style = fg_dim(muted);
        row.leading       = it.primary;
        row.leading_style = items_focus ? fg_of(fg) : fg_dim(muted);
        // Detail + hint on the trailing side: "detail   hint".
        std::string trail = it.secondary;
        if (!it.hint.empty()) {
            if (!trail.empty()) trail += "   ";
            trail += it.hint;
        }
        row.trailing       = std::move(trail);
        row.trailing_style = fg_dim(muted);
        row.selected       = items_focus && (i == o->item_index);
        cfg.rows.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    // Focus breadcrumb.
    cfg.footer.push_back(h(
        text(items_focus ? "  Items " : "  Categories ",
             fg_of(highlight)),
        text(items_focus ? "\xe2\x86\x90 back to categories"
                         : "\xe2\x86\x92 into items",
             fg_dim(muted))
    ).build());
    cfg.footer.push_back(h(
        text("\xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)),  text("move   ", fg_dim(muted)),
        text("\xe2\x86\x90\xe2\x86\x92/Tab ", fg_of(fg)), text("column   ", fg_dim(muted)),
        text("Enter ", fg_of(fg)), text("act   ", fg_dim(muted)),
        text("Esc ", fg_of(fg)),   text("close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
