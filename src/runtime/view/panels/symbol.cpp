// symbol.cpp — the #-symbol palette.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"

namespace agentty::ui {

Element symbol_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Symbol>();
    if (!o) return nothing();

    const auto& matches = o->picker.filtered();
    const auto& entries = o->picker.entries();

    Panel::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = kPanelWide;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->picker.index();

    // The sigil is the trigger character, so the header reads as a
    // continuation of what was typed in the composer.
    cfg.header.push_back(
        filter_header(o->picker.query(), "symbols", highlight, "# "));
    cfg.header.push_back(sep);

    if (entries.empty()) {
        cfg.prebuilt.push_back(text(
            o->picker.source_ready()
                ? "  no symbols indexed"
                : "  indexing symbols… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.prebuilt.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            Panel::Item row;
            // Combine symbol name + locus into the leading cell so a
            // long parent-dir trailing still has room to render; the
            // "name  file:line" pair is what the user is scanning.
            row.leading        = sym.name + "  " + std::string{fname}
                               + ":" + std::to_string(sym.line_number);
            row.leading_style  = fg_of(fg);
            // Highlight the matched chars of the symbol NAME (which is the
            // leading segment, so its offsets map directly onto row.leading).
            if (!o->picker.query().empty()) {
                auto fm = fuzzy::score(sym.name, o->picker.query());
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            cfg.items.push_back(std::move(row));
        }
    }

    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->picker.index() + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
