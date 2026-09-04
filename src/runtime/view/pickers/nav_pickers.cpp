// nav_pickers.cpp — navigation & command overlays: the thread list, the
// Smart Mode config overlay, the command palette (^K), the @-mention file
// palette, and the symbol palette. Split out of the former monolithic
// pickers.cpp; shared scaffolding lives in pickers_prologue.hpp /
// pickers_common.hpp.
//
// Pure adapter: builds maya::Picker::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements and the typed cursor index.

#include "pickers_prologue.hpp"

namespace agentty::ui {

Element thread_list(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::ThreadList>();
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Threads ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = picker->index;

    if (m.d.threads.empty()) {
        cfg.items.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    } else {
        cfg.rows.reserve(m.d.threads.size());
        int i = 0;
        for (const auto& t : m.d.threads) {
            const bool is_current = (t.id == m.d.current.id);
            const bool confirming = (picker->confirm_remove == t.id.value);
            Picker::Config::Row row;
            // "● " marks the thread you're IN — the anchor for both the
            // picker and the ^←→ / Alt+←→ quick-cycle. Non-current rows
            // get a two-space gutter so titles stay column-aligned.
            row.leading        = (is_current ? "\xe2\x97\x8f " : "  ")
                               + (t.title.empty() ? "(untitled)" : t.title);
            // Thread TITLES are what you are choosing between — full
            // foreground. Same hierarchy rule as the model/provider pickers.
            row.leading_style  = is_current ? fg_bold(info) : fg_of(fg);
            if (confirming) {
                row.badge       = "\xe2\x9a\xa0";           // ⚠
                row.badge_style = fg_of(warn);
                row.leading_style = fg_bold(warn);
                row.trailing       = "press d again to confirm";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = timestamp_full(t.updated_at);
                row.trailing_style = fg_dim(muted);
            }
            row.selected = (i == picker->index);
            // The TITLE is what you are choosing; the timestamp is reference
            // data and yields first on a narrow terminal.
            row.trailing_secondary = true;
            cfg.rows.push_back(std::move(row));
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    // Positional readout — same "k/N" the ^←→ / Alt+←→ toast shows, so
    // the two navigation surfaces speak one coordinate system.
    if (!m.d.threads.empty()) {
        cfg.footer.push_back(text(
            "  " + std::to_string(picker->index + 1) + "/"
                + std::to_string(m.d.threads.size()),
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"PgUp/PgDn", "page", 2},
        {"Enter", "open", 5},
        {"N", "new", 3},
        {"D", picker->confirm_remove.empty() ? "remove" : "confirm", 3},
        {"^/Alt+\xe2\x86\x90\xe2\x86\x92", "cycle", 1},   // ^←→ / Alt+←→
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

// Smart Mode config overlay: a master Enabled toggle + the three role slots,
// each showing its RESOLVED model (pinned, or the auto-fill). See
// docs/design/smart-mode.md.
Element smart_mode_overlay(const Model& m) {
    auto* o = m.ui.overlay.get<ov::SmartMode>();
    if (!o) return nothing();

    const auto& sm = m.d.smart;
    const std::string parent = m.d.model_id.value;

    // Resolve each role for DISPLAY (what would actually run right now).
    // Provider-scoped like the wire path, so a pin made under another provider
    // shows the auto-fill that will really serve the turn rather than a model
    // this endpoint cannot stream.
    auto shown = [&](smart::ModelRole role) -> std::string {
        auto rp = smart::resolve_role(role, parent, m.d.effort,
                                      m.d.available_models, sm,
                                      active_provider_id());
        std::string label = pretty_model_label(rp.model);
        return label.empty() ? rp.model : label;
    };
    auto slot_suffix = [&](const smart::SlotOverride& ov) -> std::string {
        return (sm.enabled && ov.set) ? "  \xc2\xb7 pinned" : "  \xc2\xb7 auto";
    };

    Picker::Config cfg;
    cfg.title      = " Smart Mode ";
    cfg.accent     = success;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.selected   = static_cast<int>(o->row);

    const bool on = sm.enabled;
    // Rows are generated BY WALKING smart::kOverlayRows — the same
    // enumeration the cursor moves through. There is no second list to keep
    // in step, so the renderer and the navigation cannot disagree about how
    // many rows exist or what each one means; that disagreement is exactly
    // what let the cursor run off the ends of this overlay.
    for (const smart::OverlayRow rr : smart::kOverlayRows) {
        const auto role = smart::role_of(rr);

        Picker::Config::Row r;
        if (!role) {
            r.leading = on ? "\xe2\x97\x8f Smart Mode" : "\xe2\x97\x8b Smart Mode";
            r.leading_style = on ? Style{}.with_fg(success).with_bold()
                                 : fg_dim(muted);
            r.trailing = smart::tuning::enabled_override()
                ? std::string{on ? "on (env pin)" : "off (env pin)"}
                : std::string{on ? "on" : "off"};
        } else {
            r.leading = "  " + std::string{smart::role_display_name(*role)};
            r.leading_style = on ? fg_of(fg) : fg_dim(muted);
            r.trailing = on ? shown(*role) + slot_suffix(sm.slot(*role))
                            : std::string{"\xe2\x80\x94"};
        }
        r.trailing_style = fg_dim(muted);
        r.selected       = (rr == o->row);
        cfg.rows.push_back(std::move(r));
    }

    cfg.footer.push_back(text(""));
    {
        // Live SESSION state — the two adaptive inputs that move the next
        // turn's route (cascade bias + tier momentum). Without this line the
        // "learning" is a black box: a route that shifted from a session
        // regret is indistinguishable from classifier noise. Only shown when
        // either is non-neutral (neutral state = no noise).
        if (sm.enabled
            && (m.s.smart_effort_bias != 0
                || m.s.smart_turn_complexity != smart::Complexity::Standard)) {
            std::string live = "  This session: ";
            if (m.s.smart_effort_bias != 0) {
                live += "effort bias ";
                live += (m.s.smart_effort_bias > 0 ? "+" : "");
                live += std::to_string(m.s.smart_effort_bias);
            }
            if (m.s.smart_turn_complexity != smart::Complexity::Standard) {
                if (m.s.smart_effort_bias != 0) live += " \xc2\xb7 ";
                live += "momentum ";
                live += smart::to_string(m.s.smart_turn_complexity);
            }
            cfg.footer.push_back(text(std::move(live), fg_dim(muted)));
        }
    }
    {
        std::vector<Hint> hints = {
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
            {"Enter", smart::is_slot_row(o->row) ? "set model" : "toggle", 4},
        };
        // `x` only acts on the three model-slot rows (1-3) — advertising it
        // on the master row promises a key that silently does nothing.
        if (smart::is_slot_row(o->row)) hints.push_back({"x", "auto", 3});
        hints.push_back({"Esc", "close", 4});
        cfg.footer.push_back(key_hints(std::move(hints)));
    }
    return Picker{std::move(cfg)}.build();
}

Element command_palette(const Model& m) {
    auto* o = m.ui.overlay.get<ov::CommandPalette>();
    if (!o) return nothing();

    // Live visibility context — the SAME predicate the reducer uses, so a row
    // the dispatcher would reject never renders (no dead Accept-all).
    PaletteContext pctx;
    pctx.update_available    = !m.s.update_latest.empty();
    pctx.has_pending_changes = !m.d.pending_changes.empty();
    pctx.has_code_block      = [&] {
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it)
            if (it->role == Role::Assistant && !it->text.empty()
                && !code_block_picker::extract_code_blocks(it->text).empty())
                return true;
        return false;
    }();
    auto scored = match_commands(o->query, pctx);
    std::vector<const CommandDef*> matches;
    matches.reserve(scored.size());
    for (const auto& s : scored) matches.push_back(s.cmd);

    Picker::Config cfg;
    cfg.title      = " Command Palette ";
    cfg.accent     = highlight;
    cfg.min_width  = 54;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.command_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(
        o->query.empty()
            ? h(text("\xe2\x8c\x98 ", fg_bold(highlight)),   // ⌘
                query_caret(highlight),
                text("type to filter\xe2\x80\xa6", fg_italic(muted))
              ).build()
            : h(text("\xe2\x8c\x98 ", fg_bold(highlight)),
                text(o->query, fg_of(fg)),
                query_caret(highlight)
              ).build());
    cfg.header.push_back(sep);

    // Each category owns a hue so the flat list reads as coloured bands; the
    // badge keeps its hue on the selected row (Picker contract), so the
    // grouping survives the cursor. General rows carry no badge (Quit/Update
    // don't need a section chip).
    auto category_hue = [](Category c) -> Color {
        switch (c) {
            case Category::Thread:   return info;
            case Category::Changes:  return success;
            case Category::Navigate: return highlight;
            case Category::Config:   return warn;
            case Category::Account:  return muted;
            case Category::General:  return muted;
        }
        return muted;
    };

    if (matches.empty()) {
        cfg.items.push_back(text(
            o->query.empty() ? "  no commands available"
                             : "  no command matches \"" + o->query + "\"",
            fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size() + 6);
        // On the EMPTY query we render real SECTION HEADERS between category
        // groups — true nesting, VS Code / Raycast style. The moment the user
        // types, headers vanish and the list goes flat-with-ranking (empty
        // categories would be noise, and label-hit ranking reorders anyway).
        // Headers are non-selectable rows; the cursor (o->index) indexes the
        // header-FREE `matches`, so we only set Row::selected on real rows and
        // point cfg.selected at the header-adjusted display position — the
        // reducer/dispatch stay entirely header-unaware.
        const bool show_headers = o->query.empty();
        Category   last_cat     = Category::General;
        bool       first_group  = true;
        int        display_row  = 0;   // position INCLUDING headers, for scroll
        int        sel_display  = -1;

        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];

            const bool group_start =
                show_headers && (first_group || cmd.category != last_cat);
            if (group_start) {
                if (auto lab = category_label(cmd.category); !lab.empty()) {
                    Picker::Config::Row hdr;
                    hdr.is_header = true;
                    // A connector header: "┌─ THREAD" so the eye reads it as the
                    // TOP of a bracket whose │ spine (below) runs down the
                    // group's rows — real visual containment, tree-style.
                    std::string up{lab};
                    for (char& ch : up) ch = static_cast<char>(std::toupper(
                        static_cast<unsigned char>(ch)));
                    hdr.leading       = "\xe2\x94\x8c\xe2\x94\x80 " + up;   // ┌─ 
                    hdr.leading_style = fg_of(category_hue(cmd.category));
                    cfg.rows.push_back(std::move(hdr));
                    ++display_row;
                }
                last_cat    = cmd.category;
                first_group = false;
            }

            Picker::Config::Row row;

            // ── Tree spine ── on the sectioned (empty-query) view each command
            // carries a │ gutter in its section's hue, so the group reads as a
            // bracket hanging off its ┌─ header. The badge cell (between the
            // cursor edge and the label) is exactly the right slot for it, and
            // it stays coloured on the selected row (Picker keeps badge hue).
            // The last row of a group closes the bracket with └.
            if (show_headers) {
                const bool group_end =
                    (i + 1 >= static_cast<int>(matches.size()))
                    || matches[static_cast<std::size_t>(i + 1)]->category != cmd.category;
                row.badge       = group_end ? "\xe2\x94\x94 "    // └ 
                                            : "\xe2\x94\x82 ";   // │ 
                row.badge_style = fg_dim(category_hue(cmd.category));
            }

            // ── Label, with live toggle/mode state folded in ──
            std::string label{cmd.label};
            if (cmd.id == Command::SmartMode)
                label += m.d.smart.enabled ? "  (on)" : "  (off)";
            else if (cmd.id == Command::ToggleChangesStrip)
                label += m.d.show_changes_strip ? "  (shown)" : "  (hidden)";
            row.leading = std::move(label);
            row.leading_style = cmd.danger ? fg_of(danger) : fg_of(fg);
            // Highlight the fuzzy-matched characters (Raycast-style) so the
            // ranking is legible: with "re" typed, the "Re" in Review/Reject
            // lights up. Positions came from the scored matcher.
            if (!o->query.empty()) {
                row.highlight    = scored[static_cast<std::size_t>(i)].positions;
                row.highlight_fg = cmd.danger ? danger : highlight;
            }

            // ── Trailing: description · shortcut. The LABEL is what you
            // select, so mark the trailing SECONDARY — the widget shrinks it
            // first and keeps a gap, so on a narrow (phone/SSH) terminal the
            // description gracefully truncates (then vanishes) while the label
            // and shortcut stay whole, instead of the label being eaten.
            std::string trailing{cmd.description};
            if (cmd.shortcut && *cmd.shortcut) {
                trailing += "  \xc2\xb7  ";
                trailing += cmd.shortcut;
            }
            row.trailing           = std::move(trailing);
            row.trailing_style     = fg_dim(muted);
            row.trailing_secondary = true;
            row.selected = (i == o->index);
            if (i == o->index) sel_display = display_row;
            cfg.rows.push_back(std::move(row));
            ++display_row;
        }
        // Scroll tracks the header-adjusted cursor position.
        cfg.selected = sel_display;
    }

    cfg.footer.push_back(text(""));
    // Count anchor when the list is scrolled — same grammar as the @ / # pickers.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()) + " commands",
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"type", "filter", 3},
        {"Enter", "run", 6},
        {"Esc", "close", 3},
    }));

    return Picker{std::move(cfg)}.build();
}

Element mention_palette(const Model& m) {
    auto* o = m.ui.overlay.get<ov::Mention>();
    if (!o) return nothing();

    const auto& matches = mention_filtered(*o);

    Picker::Config cfg;
    cfg.title      = " Mention File ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.mention_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("@", fg_bold(info)),
        text(o->query.empty() ? " your changed files first · type to filter…"
                              : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->files.empty()) {
        // Distinguish "still indexing" from "genuinely empty" — the walk
        // runs on a background thread; if it hasn't landed the picker
        // opened with an empty snapshot. files_ready() tells them apart.
        cfg.items.push_back(text(
            files_ready() ? "  workspace empty (or no readable files)"
                          : "  indexing workspace… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            Picker::Config::Row row;
            // Git-status badge — the working-set signal, colour-coded so the
            // file you're editing is unmistakable at a glance. Padded to a
            // fixed width so leading text aligns across rows.
            if (auto tag = file_git_tag(path); tag != GitTag::None) {
                auto label = git_tag_label(tag);
                row.badge = "● " + std::string{label};
                row.badge_style =
                    tag == GitTag::Modified          ? fg_of(maya::Color::yellow())
                  : tag == GitTag::Staged            ? fg_of(maya::Color::green())
                  : tag == GitTag::Untracked         ? fg_of(info)
                  : /* RecentlyCommitted */            fg_dim(muted);
            }
            row.leading        = std::string{name};
            row.leading_style  = fg_of(fg);
            // Light up the matched characters of the filename so the fuzzy
            // rank is legible (re-score the name in-view against the query;
            // the workspace scorer ranks but doesn't return positions).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == o->index);
            cfg.rows.push_back(std::move(row));
        }
    }

    // Position indicator: still useful as a textual N/total anchor even
    // though the scrollbar shows the same thing visually.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Picker{std::move(cfg)}.build();
}

Element symbol_palette(const Model& m) {
    auto* o = m.ui.overlay.get<ov::Symbol>();
    if (!o) return nothing();

    const auto& matches = symbol_filtered(*o);

    Picker::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("#", fg_bold(highlight)),
        text(o->query.empty() ? " type to filter symbols…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->entries.empty()) {
        cfg.items.push_back(text(
            symbols_ready() ? "  no symbols indexed"
                            : "  indexing symbols… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = o->entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            Picker::Config::Row row;
            // Combine symbol name + locus into the leading cell so a
            // long parent-dir trailing still has room to render; the
            // "name  file:line" pair is what the user is scanning.
            row.leading        = sym.name + "  " + std::string{fname}
                               + ":" + std::to_string(sym.line_number);
            row.leading_style  = fg_of(fg);
            // Highlight the matched chars of the symbol NAME (which is the
            // leading segment, so its offsets map directly onto row.leading).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(sym.name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == o->index);
            cfg.rows.push_back(std::move(row));
        }
    }

    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
