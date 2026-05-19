#include "agentty/runtime/view/pickers.hpp"

#include <algorithm>
#include <vector>

#include <maya/widget/picker.hpp>
#include <maya/widget/plan_view.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/workspace/files.hpp"
#include "agentty/workspace/symbols.hpp"

// Pure adapter: builds maya::Picker::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements (each picker formats its items
// differently — favourite stars, timestamps, parent-dir disambiguators)
// and the typed cursor index.
//
// Per-row truncation rides on `text(...) | clip` (TextWrap::TruncateEnd):
// maya measures the column it allocated to the row and returns a
// truncated-with-ellipsis single line if the natural content overflows.

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// "src/runtime/foo.cpp" → ("foo.cpp", "src/runtime/").
// Returns ("foo.cpp", "") for a bare filename.
std::pair<std::string_view, std::string_view>
split_name_dir(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return {path, {}};
    return {path.substr(slash + 1), path.substr(0, slash + 1)};
}

// Compress a directory path to its IMMEDIATE parent only — that's
// the disambiguator the user actually scans for. Truncation of the
// segment itself is left to maya (`| clip`), so this just performs
// the semantic step ("/home/.../Best Of Kumar Sanu/" → "Kumar Sanu/").
std::string parent_segment(std::string_view dir) {
    if (dir.empty()) return {};
    auto inner = dir;
    if (inner.back() == '/') inner.remove_suffix(1);
    auto slash = inner.find_last_of('/');
    auto last = (slash == std::string_view::npos)
        ? inner : inner.substr(slash + 1);
    std::string out{last};
    out.push_back('/');
    return out;
}

// Viewport height (rows) for every picker's scrollable list. Single
// constant so all pickers share the same shape. Items beyond this
// reachable via the scrollbar; selection always stays visible via
// the widget's auto-scroll-to-selection logic.
constexpr int kViewportH = 14;

// U+258E LEFT ONE QUARTER BLOCK — thin vertical bar pinned at col 0.
// Convention every modern editor uses for "current" / "selected" row.
constexpr const char* kEdgeBar = "\xe2\x96\x8e";

} // namespace

Element model_picker(const Model& m) {
    auto* picker = pick::opened(m.ui.model_picker);
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Models ";
    cfg.accent     = accent;
    cfg.min_width  = 40;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.model_picker_scroll;
    cfg.selected   = picker->index;

    if (m.d.available_models.empty()) {
        cfg.items.push_back(text("  Loading models…", fg_italic(muted)));
    } else {
        cfg.items.reserve(m.d.available_models.size());
        int i = 0;
        for (const auto& mi : m.d.available_models) {
            bool sel    = i == picker->index;
            bool active = mi.id == m.d.model_id;
            // Two orthogonal affordances, both at col 0 — no bg fill:
            //   • CURSOR (transient): cyan left bar + bright_white bold
            //     text. Follows the arrow keys.
            //   • ACTIVE (persistent): magenta left bar. Marks the model
            //     currently in use, regardless of cursor.
            //
            // When cursor IS on the active row, cursor wins the bar
            // colour (cyan) but the text stays bright_white bold —
            // the user already knows it's active because Enter would
            // be a no-op. When cursor is elsewhere, the magenta bar
            // stays put so the user sees what they'd be switching FROM.
            //
            // No background fill: the heavy gray bar was visually
            // chunky and competed with the text. A single column of
            // colour reads instantly and leaves the row legible.
            auto edge_bar = sel
                ? text(kEdgeBar, fg_bold(highlight))
                : active
                    ? text(kEdgeBar, fg_bold(accent))
                    : text(" ");
            auto star = mi.favorite ? text(" ★", fg_of(warn)) : text("  ");
            cfg.items.push_back(h(
                edge_bar,
                text(" "),
                text(mi.display_name,
                     sel    ? fg_bold(maya::Color::bright_white())
                     : active ? fg_bold(fg)
                              : fg_of(muted)) | clip | grow(1.0f),
                star
            ).build());
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("↑↓", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" select  ", fg_dim(muted)),
        text("F", fg_of(fg)), text(" favorite  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

Element thread_list(const Model& m) {
    auto* picker = pick::opened(m.ui.thread_list);
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Threads ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = picker->index;

    if (m.d.threads.empty()) {
        cfg.items.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    } else {
        cfg.items.reserve(m.d.threads.size());
        int i = 0;
        for (const auto& t : m.d.threads) {
            bool sel = i == picker->index;
            // Selection: cyan left bar at col 0 + bright_white bold
            // title. No bg fill — keeps the row legible against the
            // surrounding chrome. Timestamp stays muted; the bar is
            // doing the cursor work, the timestamp keeps its column.
            //
            // Row uses hstack().width(100%) so spacer() can absorb
            // leftover space and push the timestamp flush right (a
            // plain h(...).build() sizes to natural content width
            // and spacer() has nothing to grow into — see
            // messenger.cpp build_header() comment).
            auto edge_bar = sel
                ? text(kEdgeBar, fg_bold(highlight))
                : text(" ");
            cfg.items.push_back(hstack()
                .width(Dimension::percent(100))(
                edge_bar,
                text(" "),
                text(t.title.empty() ? "(untitled)" : t.title,
                     sel ? fg_bold(maya::Color::bright_white())
                         : fg_of(muted)) | clip,
                spacer(),
                text(timestamp_full(t.updated_at),
                     sel ? fg_of(fg) : fg_dim(muted)),
                text(" ")));
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("↑↓", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" open  ", fg_dim(muted)),
        text("N", fg_of(fg)), text(" new  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

Element command_palette(const Model& m) {
    auto* o = opened(m.ui.command_palette);
    if (!o) return nothing();

    auto matches = filtered_commands(o->query);

    Picker::Config cfg;
    cfg.title      = " Command Palette ";
    cfg.accent     = highlight;
    cfg.min_width  = 50;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.command_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("› ", fg_bold(highlight)),
        text(o->query.empty() ? "type to filter…" : o->query,
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];
            bool sel = i == o->index;
            auto edge_bar = sel
                ? text(kEdgeBar, fg_bold(highlight))
                : text(" ");
            cfg.items.push_back(hstack()
                .width(Dimension::percent(100))(
                edge_bar,
                text(" "),
                text(std::string{cmd.label},
                     sel ? fg_bold(maya::Color::bright_white())
                         : fg_of(muted)) | clip,
                spacer(),
                text(std::string{cmd.description},
                     sel ? fg_of(fg) : fg_dim(muted)) | clip,
                text(" ")));
        }
    }

    return Picker{std::move(cfg)}.build();
}

Element mention_palette(const Model& m) {
    auto* o = mention_opened(m.ui.mention_palette);
    if (!o) return nothing();

    auto matches = filter_files(o->files, o->query);

    Picker::Config cfg;
    cfg.title      = " Mention File ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.mention_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("@", fg_bold(info)),
        text(o->query.empty() ? " type to filter files…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->files.empty()) {
        cfg.items.push_back(text("  workspace empty (or no readable files)", fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            bool sel = i == o->index;
            auto edge_bar = sel
                ? text(kEdgeBar, fg_bold(highlight))
                : text(" ");
            cfg.items.push_back(hstack()
                .width(Dimension::percent(100))(
                edge_bar,
                text(" "),
                text(std::string{name},
                     sel ? fg_bold(maya::Color::bright_white())
                         : fg_of(fg)) | clip,
                spacer(),
                text(parent_segment(dir),
                     sel ? fg_of(fg) : fg_dim(muted)) | clip,
                text(" ")));
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
    auto* o = symbol_palette_opened(m.ui.symbol_palette);
    if (!o) return nothing();

    auto matches = filter_symbols(o->entries, o->query);

    Picker::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = 60;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("#", fg_bold(highlight)),
        text(o->query.empty() ? " type to filter symbols…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->entries.empty()) {
        cfg.items.push_back(text("  no symbols indexed", fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.items.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = o->entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            bool sel = i == o->index;
            std::string locus = std::string{fname} + ":"
                              + std::to_string(sym.line_number);
            auto edge_bar = sel
                ? text(kEdgeBar, fg_bold(highlight))
                : text(" ");
            cfg.items.push_back(hstack()
                .width(Dimension::percent(100))(
                edge_bar,
                text(" "),
                text(sym.name,
                     sel ? fg_bold(maya::Color::bright_white())
                         : fg_of(fg)) | clip,
                text("  "),
                text(locus,
                     sel ? fg_of(fg) : fg_dim(muted)) | clip,
                spacer(),
                text(parent_segment(dir),
                     sel ? fg_of(fg) : fg_dim(muted)) | clip,
                text(" ")));
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

Element todo_modal(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return nothing();

    Picker::Config cfg;
    cfg.title      = " Plan ";
    cfg.accent     = info;
    cfg.min_width  = 45;
    cfg.viewport_h = kViewportH;
    cfg.scroll     = &m.ui.todo_scroll;
    // No selection cursor in the todo modal — read-only. Pass -1
    // so the auto-scroll-to-selection is a no-op and the user's
    // manual scroll position is fully respected.
    cfg.selected   = -1;

    if (m.ui.todo.items.empty()) {
        cfg.items.push_back(text("  No tasks yet.", fg_italic(muted)));
        cfg.items.push_back(text("  The agent will create tasks as it works.", fg_dim(muted)));
    } else {
        // PlanView returns one Element with all tasks. It lives in
        // the scrollable region so a long task list pages cleanly
        // when it overflows the viewport.
        maya::PlanView plan;
        for (const auto& item : m.ui.todo.items) {
            maya::TaskStatus ts;
            switch (item.status) {
                case TodoStatus::Pending:    ts = maya::TaskStatus::Pending; break;
                case TodoStatus::InProgress: ts = maya::TaskStatus::InProgress; break;
                case TodoStatus::Completed:  ts = maya::TaskStatus::Completed; break;
            }
            plan.add(item.content, ts);
        }
        cfg.items.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        cfg.footer.push_back(text(""));
        cfg.footer.push_back(h(
            text("  " + std::to_string(done_count) + "/" + std::to_string(total),
                 fg_bold(done_count == total ? success : info)),
            text(" completed", fg_dim(muted))
        ).build());
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
