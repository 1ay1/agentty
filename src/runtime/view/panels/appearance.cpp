// appearance.cpp — the Appearance pane (theme, density, motion, chrome).
//
// Two surfaces in one view:
//
//   1. The pane itself — a plain form, projected through the shared
//      form_config exactly as Smart Mode and the plugin editor are. No
//      chrome here is appearance-specific; that is the point of the shared
//      layer.
//
//   2. The theme browser — a floating list, rendered OVER the pane rather
//      than instead of it. A theme picker that covers the screen it is
//      restyling asks you to judge a scheme by its name, so this one is
//      deliberately narrow and bottom-anchored: the transcript, the
//      composer and the pane behind it are all repainted in the
//      highlighted scheme, and the list is a caption on its own preview.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"
#include "agentty/runtime/panel/appearance.hpp"

namespace agentty::ui {

namespace {

// The browser, floating. Narrow on purpose (kPanelNarrow): every column it
// does not take is a column of real UI left visible in the scheme you are
// looking at.
[[nodiscard]] Element theme_browser(const Model& m, const pn::Appearance& o) {
    const auto& names = pn::matching_themes(o.pane.picker.query);

    Panel::Config cfg;
    cfg.title      = " Theme ";
    cfg.accent     = info;
    cfg.min_width  = kPanelNarrow;
    // Half the usual viewport. The list is the smaller half of what is on
    // screen — what it is previewing is the larger.
    cfg.viewport_h = std::max(6, panel_viewport_h() / 2);
    cfg.scroll     = &m.ui.appearance_scroll;
    cfg.selected   = names.empty() ? -1 : o.pane.picker.index;

    cfg.header.push_back(filter_header(o.pane.picker.query));
    cfg.header.push_back(sep);

    if (names.empty()) {
        cfg.prebuilt.push_back(text("  no scheme matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(names.size());
        for (const auto& name : names) {
            Panel::Item row;
            // The empty name IS native — the terminal's own colours, and the
            // only choice correct on a light terminal, a 16-colour terminal
            // and a monochrome one alike. It says so, because "native" on
            // its own reads like just another scheme.
            const bool is_native = name.empty();
            row.leading       = is_native ? "native" : name;
            row.leading_style = is_native ? fg_bold(fg) : fg_of(fg);
            if (is_native) {
                row.trailing       = "your terminal's own colours";
                row.trailing_style = fg_dim(muted);
            }
            // The palette itself, which is the thing the row is actually
            // about. A name is the least informative property of a colour
            // scheme — "Rose Pine Dawn" does not tell you it is light, and
            // "Spacedust" does not tell you it is warm — so the six slots
            // that most characterise a scheme are painted next to it: the
            // three accents you see constantly, then the status hues.
            //
            // native is deliberately left blank. Its colours are whatever
            // the terminal's are, so a swatch would be a claim maya cannot
            // make; the trailing text says that in words instead.
            if (!is_native) {
                if (const maya::Theme* t = ui_prefs::find_scheme(name)) {
                    row.swatch = {t->primary, t->accent, t->info,
                                  t->success, t->warning, t->error};
                }
            }
            if (!o.pane.picker.query.empty() && !is_native) {
                auto fm = fuzzy::score(name, o.pane.picker.query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            cfg.items.push_back(std::move(row));
        }
    }

    cfg.footer.push_back(sep);
    cfg.footer.push_back(text(
        "  Moving applies it \xe2\x80\x94 you are looking at the preview.",
        fg_dim(muted)));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "preview", 5},   // ↑↓
        {"Enter", "keep", 3},
        {"Esc", "revert", 4},
    }));

    return Panel{std::move(cfg)}.build();
}

}  // namespace

Element appearance_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Appearance>();
    if (!o) return nothing();
    if (o->pane.picking) return theme_browser(m, *o);
    return maya::Panel{form_config(o->pane.form, info,
                                   &m.ui.appearance_scroll,
                                   panel_viewport_h(),
                                   panel_terminal_cols())}.build();
}

}  // namespace agentty::ui
