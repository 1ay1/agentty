// thread_list.cpp - the thread switcher panel (^J): pick, ^D delete, ^R rename.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include "thread_time.hpp"
#include "agentty/runtime/panel/thread_filter.hpp"   // visible_threads + fuzzy
#include "agentty/runtime/panel/thread_peek.hpp"

#include <maya/widget/peek_card.hpp>

#include <filesystem>
#include <optional>

namespace agentty::ui {

namespace {

// The folder's own name, for a title or a row: "/home/a/agentty" -> "agentty".
[[nodiscard]] std::string workspace_name(std::string_view path) {
    std::filesystem::path p{std::string{path}};
    if (!p.has_filename()) p = p.parent_path();   // trailing slash
    std::string n = p.filename().string();
    return n.empty() ? std::string{path} : n;
}

using PeekLine = maya::PeekCard::Line;
using PeekKind = maya::PeekCard::Kind;

[[nodiscard]] PeekLine to_card(const ui::PeekLine& l) {
    using K = ui::PeekLine::Kind;
    return {l.who, l.text, l.kind == K::User ? PeekKind::User
                         : l.kind == K::Tool ? PeekKind::Tool : PeekKind::Reply};
}

// The preview for the thread under the cursor. Built from the turns cached
// when it loaded, so it's a few dozen string copies a frame, never a parse.
// What makes a thread recognisable goes first: its name, when, how long,
// where it ran and what you first asked. The turns scrolled to fill the rest.
[[nodiscard]] maya::PeekCard::Config peek_for(const Model& m, const pn::ThreadList& p,
                                              const Thread& meta, int rows) {
    maya::PeekCard::Config pc;
    pc.rows  = rows;
    pc.title = std::string{ui::shown_title(meta)};
    const auto now = m.wall_at(maya::anim_now());
    pc.facts = short_age(meta.updated_at, now) + " ago";
    if (meta.message_count > 0) pc.facts += " \xc2\xb7 " + std::to_string(meta.message_count) + " msgs";
    std::vector<std::string> sub;
    if (!meta.workspace.empty()) sub.push_back(workspace_name(meta.workspace));
    if (!meta.forked_from.empty()) sub.push_back("\xe2\x91\x82 fork");   // ⑂
    if (meta.auto_titled && !meta.user_named()) sub.push_back("auto-named");
    for (std::size_t i = 0; i < sub.size(); ++i)
        pc.subtitle += (i ? " \xc2\xb7 " : "") + sub[i];

    const auto loaded = std::ranges::find(m.ui.peek_cache, meta.id,
                                          &Model::UI::PeekThread::id);
    // The opening question, pinned at the top: the fastest way to know
    // which conversation this was. From the index while the load is out.
    std::string asked = meta.preview;
    if (asked.empty() && loaded != m.ui.peek_cache.end() && !loaded->turns.empty()
        && !loaded->turns.front().empty()
        && loaded->turns.front().front().kind == ui::PeekLine::Kind::User)
        asked = loaded->turns.front().front().text;
    // Not when it already is the title (an untitled or broken-name thread).
    if (asked == pc.title) asked.clear();
    pc.asked = std::move(asked);

    if (loaded == m.ui.peek_cache.end()) {
        pc.lines.push_back({"", "loading\xe2\x80\xa6", PeekKind::Note});
        pc.hint = "Enter open";
        return pc;
    }
    const auto& turns = loaded->turns;
    if (turns.empty()) {
        pc.lines.push_back({"", "nothing here yet", PeekKind::Note});
        pc.hint = "Enter open";
        return pc;
    }
    const int n = static_cast<int>(turns.size());
    const int last = n - 1 - std::clamp(p.peek_turn, 0, n - 1);
    // Lines the card has for the transcript, as the card itself counts them.
    const int room = maya::PeekCard::transcript_rows(pc);
    // Walk back from the scrolled-to turn until the card is full, so the
    // newest turn in view sits at the bottom of the card.
    std::vector<PeekLine> lines;
    for (int ti = last; ti >= 0 && static_cast<int>(lines.size()) < room; --ti) {
        std::vector<PeekLine> chunk;
        for (const auto& l : turns[static_cast<std::size_t>(ti)]) chunk.push_back(to_card(l));
        if (ti != last) chunk.push_back({"", "", PeekKind::Gap});
        // A turn taller than the card shows its start: the question.
        if (ti == last && static_cast<int>(chunk.size()) > room)
            chunk.resize(static_cast<std::size_t>(std::max(0, room)));
        lines.insert(lines.begin(), chunk.begin(), chunk.end());
    }
    if (static_cast<int>(lines.size()) > room)
        lines.erase(lines.begin(), lines.end() - std::max(0, room));
    while (!lines.empty() && lines.front().kind == PeekKind::Gap) lines.erase(lines.begin());
    pc.lines.insert(pc.lines.end(), lines.begin(), lines.end());
    pc.hint = "\xe2\x86\x90 \xe2\x86\x92 turn " + std::to_string(last + 1) + "/"
            + std::to_string(n) + " \xc2\xb7 Enter open";
    return pc;
}

} // namespace

Element thread_list_panel(const Model& m) {
    auto* picker = m.ui.panel.get<pn::ThreadList>();
    if (!picker) return nothing();
    const auto visible = ui::visible_threads(m.d.threads, picker->query, m.env.cwd,
                                             picker->all_workspaces);
    const bool all = ui::effective_all(m.d.threads, m.env.cwd, picker->all_workspaces);
    const int  here = ui::workspace_thread_count(m.d.threads, m.env.cwd);
    const bool renaming = !picker->renaming.empty();

    Panel::Config cfg;
    cfg.title      = all ? " Threads \xc2\xb7 all " : " Threads \xc2\xb7 " + workspace_name(m.env.cwd) + " ";
    cfg.accent     = info;
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = -1;   // set below, past the day headers

    // The preview: a framed card fixed under the filter's rule. Same height
    // whatever thread is under the cursor, so the list below it never
    // moves. The panel has a fixed height (it must fit the frame), so the
    // card takes its rows out of the list's, keeping at least kMinList rows
    // of list. Appearance > Thread preview turns it off.
    constexpr int kPeekRows = 12, kMinList = 6;
    const int list_rows = cfg.viewport_h;
    const int peek_rows = std::min(kPeekRows, list_rows - kMinList);
    std::optional<maya::PeekCard::Config> peek;
    if (m.d.ui().thread_preview && !renaming && peek_rows >= 7
        && !picker->peek_id.empty()) {
        const auto meta = std::ranges::find(m.d.threads, picker->peek_id,
                                            [](const Thread& t) { return t.id.value; });
        if (meta != m.d.threads.end()) {
            // The rule above it costs a row; the card covers the blank row
            // under it, so it is as tall as the rows it takes.
            peek = peek_for(m, *picker, *meta, peek_rows);
            cfg.viewport_h = list_rows - peek_rows;
            cfg.fixed_viewport = true;   // the list keeps its height too
        }
    }
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
        cfg.header.push_back(sep);   // the rule under the filter, like every picker
        if (peek) cfg.header.push_back(maya::PeekCard{std::move(*peek)}.build());
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
        cfg.items.reserve(visible.size() + 5);
        const auto now = m.wall_at(maya::anim_now());
        // Day headers only on the plain list: a search ranks by match, and
        // bands would split the ranking up.
        const bool bands = picker->query.empty();
        std::optional<ui::DayGroup> band;
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
            const auto& t = m.d.threads[static_cast<std::size_t>(visible[static_cast<std::size_t>(i)])];
            if (bands) {
                const auto g = ui::day_group(t.updated_at, now);
                if (g != band) {
                    cfg.items.push_back(section_header(
                        SectionHeader{ui::day_group_label(g), muted}));
                    band = g;
                }
            }
            const bool is_current = (t.id == m.d.current.id);
            const bool confirming = (picker->confirm_remove == t.id.value);
            const bool on_cursor  = (i == picker->index);
            Panel::Item row;
            // "● " marks the thread you're IN, "⑂ " a fork. Others get a
            // two-space gutter so titles stay column-aligned.
            const char* mark = is_current            ? "\xe2\x97\x8f "    // ●
                             : !t.forked_from.empty() ? "\xe2\x91\x82 "   // ⑂
                             :                          "  ";
            row.leading        = std::string{mark} + std::string{ui::shown_title(t)};
            // Thread TITLES are what you are choosing between — full
            // foreground. Same hierarchy rule as the model/provider pickers.
            row.leading_style  = is_current ? fg_bold(info) : fg_of(fg);
            // Show why a row matched. Offsets shift by the mark in front.
            if (!picker->query.empty())
                if (auto fm = fuzzy::score(ui::shown_title(t), picker->query); fm.matched()) {
                    const int shift = static_cast<int>(std::string_view{mark}.size());
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
                std::string tail = t.auto_titled && !t.user_named() ? "auto \xc2\xb7 " : "";
                if (t.message_count > 0) tail += std::to_string(t.message_count) + " msgs \xc2\xb7 ";
                row.trailing       = tail + ui::short_age(t.updated_at, now);
                row.trailing_style = fg_dim(muted);
            }
            // The TITLE is what you are choosing; the timestamp is reference
            // data and yields first on a narrow terminal.
            row.trailing_secondary = true;
            if (on_cursor) cfg.selected = static_cast<int>(cfg.items.size());
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
    std::vector<Hint> keys{
        {"type", "filter", 5},
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
    };
    // Only offer a scope to switch to when this folder has threads.
    if (here > 0) keys.push_back({"Tab", all ? "this folder" : "all", 3});
    keys.push_back({"Enter", "open", 5});
    keys.push_back({"^N", "new", 3});
    keys.push_back({"^D", picker->confirm_remove.empty() ? "remove" : "confirm", 3});
    keys.push_back({"^R", "rename", 3});
    keys.push_back({"Esc", picker->query.empty() ? "close" : "clear", 4});
    cfg.footer.push_back(key_hints(std::move(keys)));

    return Panel{std::move(cfg)}.build();
}


} // namespace agentty::ui
