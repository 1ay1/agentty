// thread_list.cpp - the thread switcher panel (^J): pick, ^D delete, ^R rename.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include "agentty/runtime/panel/thread_filter.hpp"   // visible_threads + fuzzy

namespace agentty::ui {



Element thread_list_panel(const Model& m) {
    auto* picker = m.ui.panel.get<pn::ThreadList>();
    if (!picker) return nothing();
    const auto visible = ui::visible_threads(m.d.threads, picker->query);
    const bool renaming = !picker->renaming.empty();

    Panel::Config cfg;
    cfg.title      = " Threads ";
    cfg.accent     = info;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = visible.empty() ? -1 : picker->index;
    if (renaming) {
        // The draft takes the filter's place: a bright line with a REAL
        // caret, same as the settings add-prompt. The highlighted row below
        // stays lit so you can see which thread it renames.
        std::size_t at = std::string::npos;
        const std::string shown = maya::panel::detail::with_caret(
            picker->rename_input,
            static_cast<std::size_t>(std::max(0, picker->rename_cursor)),
            panel_terminal_cols() - 14, &at);
        const auto caret = Style{}.with_fg(info).with_conceal()
                                  .with_caret_anchor().with_caret_shape(1);
        cfg.header.push_back(h(
            text("  rename  ", fg_of(info)),
            text(shown.substr(0, at), fg_bold(fg)),
            text(shown.substr(at, 3), caret),
            text(shown.substr(at + 3), fg_bold(fg))
        ).build());
        cfg.header.push_back(sep);
    } else if (!m.d.threads.empty()) {
        cfg.header.push_back(filter_header(picker->query, "threads"));
        cfg.header.push_back(sep);
    }

    if (m.d.threads.empty()) {
        cfg.prebuilt.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    } else if (visible.empty()) {
        cfg.prebuilt.push_back(text(
            "  no thread matches \"" + picker->query + "\"", fg_italic(muted)));
    } else {
        cfg.items.reserve(visible.size());
        for (const int vi : visible) {
            const auto& t = m.d.threads[static_cast<std::size_t>(vi)];
            const bool is_current = (t.id == m.d.current.id);
            const bool confirming = (picker->confirm_remove == t.id.value);
            Panel::Item row;
            // "● " marks the thread you're IN — the anchor for both the
            // picker and the ^←→ / Alt+←→ quick-cycle. Non-current rows
            // get a two-space gutter so titles stay column-aligned.
            row.leading        = (is_current ? "\xe2\x97\x8f " : "  ")
                               + (t.title.empty() ? "(untitled)" : t.title);
            // Thread TITLES are what you are choosing between — full
            // foreground. Same hierarchy rule as the model/provider pickers.
            row.leading_style  = is_current ? fg_bold(info) : fg_of(fg);
            // Show why a row matched. Offsets shift by the 2-byte gutter /
            // 4-byte ● marker in front of the title.
            if (!picker->query.empty() && !t.title.empty())
                if (auto fm = fuzzy::score(t.title, picker->query); fm.matched()) {
                    const int shift = is_current ? 4 : 2;
                    for (int& p : fm.positions) p += shift;
                    row.highlight    = std::move(fm.positions);
                    row.highlight_fg = info;
                }
            if (confirming) {
                row.badge       = "\xe2\x9a\xa0";           // ⚠
                row.badge_style = fg_of(warn);
                row.leading_style = fg_bold(warn);
                row.trailing       = "press ^D again to confirm";
                row.trailing_style = fg_of(warn);
            } else {
                // A model-written name says so, so it doesn't pass for one
                // you typed. ^R or /rename makes it yours.
                row.trailing       = (t.auto_titled && !t.user_named()
                                          ? std::string{"auto \xc2\xb7 "} : std::string{})
                                   + timestamp_full(t.updated_at);
                row.trailing_style = fg_dim(muted);
            }
            // The TITLE is what you are choosing; the timestamp is reference
            // data and yields first on a narrow terminal.
            row.trailing_secondary = true;
            cfg.items.push_back(std::move(row));
        }
    }

    cfg.footer.push_back(text(""));
    // Positional readout — same "k/N" the ^←→ / Alt+←→ toast shows, so
    // the two navigation surfaces speak one coordinate system.
    if (!visible.empty()) {
        cfg.footer.push_back(text(
            "  " + std::to_string(picker->index + 1) + "/"
                + std::to_string(visible.size())
                + (picker->query.empty() ? std::string{}
                       : " of " + std::to_string(m.d.threads.size())),
            fg_dim(muted)));
    }
    // Armed delete is a MODE: hint it in the note line, exactly like the
    // account list and the settings add-prompt, so every two-step confirm
    // in the app reads the same way.
    if (!picker->confirm_remove.empty())
        cfg.note = "^D confirm delete \xc2\xb7 any other key cancels";
    if (renaming) {
        // Saving an emptied draft clears the name, back to the automatic
        // one; say so while the draft is empty, so the key is findable.
        const bool emptied = picker->rename_input.empty();
        cfg.footer.push_back(key_hints({
            {"type", "edit title", 5},
            {"\xe2\x86\x90\xe2\x86\x92", "move", 4},   // left/right arrows
            {"Enter", emptied ? "use automatic name" : "save", 5},
            {"Esc", "cancel", 5},
        }));
        return Panel{std::move(cfg)}.build();
    }
    cfg.footer.push_back(key_hints({
        {"type", "filter", 5},
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"Enter", "open", 5},
        {"^N", "new", 3},
        {"^D", picker->confirm_remove.empty() ? "remove" : "confirm", 3},
        {"^R", "rename", 3},
        {"^/Alt+\xe2\x86\x90\xe2\x86\x92", "cycle", 1},   // ^←→ / Alt+←→
        {"Esc", picker->query.empty() ? "close" : "clear", 4},
    }));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
