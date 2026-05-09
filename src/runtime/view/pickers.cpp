#include "moha/runtime/view/pickers.hpp"

#include <algorithm>
#include <vector>

#include <maya/widget/plan_view.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

// All sizing here is responsive — the public maya layout does the
// math, never moha. Each picker is a `vstack()` with:
//
//   .width(Dimension::percent(N))        — grows with parent (terminal)
//   .min_width(Dimension::fixed(MIN))    — readable on narrow terms
//   .max_width(Dimension::fixed(MAX))    — bounded so a 4K terminal
//                                          doesn't get a 200-col modal
//
// Per-row truncation rides on `text(...) | clip` (TextWrap::TruncateEnd):
// maya measures the column it allocated to the text, returns a
// truncated-with-ellipsis line if the natural content overflows.
// moha never queries terminal size and never recomputes column
// widths — that's maya's job and lives there.

namespace moha::ui {

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

// Wrap the rows + the picker's container chrome in a single vstack
// builder. Single source of truth for the responsive sizing recipe;
// each picker passes its own min / max / percent + accent color +
// border title and gets the same shape back.
struct PickerShape {
    int   pct_w   = 80;   // % of available width
    int   min_w   = 50;   // never narrower
    int   max_w   = 120;  // never wider
    Color accent  = fg;
    std::string title;
};

Element wrap_picker(PickerShape s, std::vector<Element> rows) {
    return vstack()
        .padding(1, 2)
        .width(Dimension::percent(static_cast<float>(s.pct_w)))
        .min_width(Dimension::fixed(s.min_w))
        .max_width(Dimension::fixed(s.max_w))
        .border(BorderStyle::Round)
        .border_color(s.accent)
        .border_text(s.title, BorderTextPos::Top, BorderTextAlign::Center)
        (rows);
}

// Visible-row cap. Same value across pickers — keeps the overlay
// from pushing the composer off the screen. Rows beyond this stay
// reachable via the cursor; only the rendered slice shifts.
constexpr int kVisible = 14;

} // namespace

Element model_picker(const Model& m) {
    auto* picker = pick::opened(m.ui.model_picker);
    if (!picker) return nothing();
    std::vector<Element> rows;
    if (m.d.available_models.empty()) {
        rows.push_back(text("  Loading models…", fg_italic(muted)));
    }
    int i = 0;
    for (const auto& mi : m.d.available_models) {
        bool sel    = i == picker->index;
        bool active = mi.id == m.d.model_id;
        auto prefix = sel ? text("› ", fg_bold(accent)) : text("  ");
        auto star   = mi.favorite ? text("★ ", fg_of(warn)) : text("  ");
        auto active_mark = active ? text(" ✓", fg_of(success)) : text("");
        rows.push_back(h(prefix, star,
            text(mi.display_name,
                 sel ? fg_bold(fg) : fg_of(muted)) | clip,
            active_mark).build());
        ++i;
    }
    rows.push_back(text(""));
    rows.push_back(h(
        text("↑↓", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" select  ", fg_dim(muted)),
        text("F", fg_of(fg)), text(" favorite  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());
    return wrap_picker({.pct_w = 50, .min_w = 40, .max_w = 80,
                        .accent = accent, .title = " Models "},
                       std::move(rows));
}

Element thread_list(const Model& m) {
    auto* picker = pick::opened(m.ui.thread_list);
    if (!picker) return nothing();
    std::vector<Element> rows;
    if (m.d.threads.empty()) {
        rows.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    }
    int i = 0;
    for (const auto& t : m.d.threads) {
        bool sel = i == picker->index;
        auto prefix = sel ? text("› ", fg_bold(info)) : text("  ");
        rows.push_back(h(prefix,
            text(t.title.empty() ? "(untitled)" : t.title,
                 sel ? fg_of(fg) : fg_of(muted)) | clip,
            spacer(),
            text(timestamp_hh_mm(t.updated_at), fg_dim(muted))
        ).build());
        if (++i > 15) break;
    }
    rows.push_back(text(""));
    rows.push_back(h(
        text("↑↓", fg_of(fg)), text(" move  ", fg_dim(muted)),
        text("Enter", fg_of(fg)), text(" open  ", fg_dim(muted)),
        text("N", fg_of(fg)), text(" new  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());
    return wrap_picker({.pct_w = 60, .min_w = 50, .max_w = 110,
                        .accent = info, .title = " Threads "},
                       std::move(rows));
}

Element command_palette(const Model& m) {
    auto* o = opened(m.ui.command_palette);
    if (!o) return nothing();

    std::vector<Element> rows;
    rows.push_back(h(text("› ", fg_bold(highlight)),
        text(o->query.empty() ? "type to filter…" : o->query,
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);

    auto matches = filtered_commands(o->query);
    if (matches.empty()) {
        rows.push_back(text("  no matches", fg_italic(muted)));
    } else {
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];
            bool sel = i == o->index;
            auto prefix = sel ? text("› ", fg_bold(highlight)) : text("  ");
            rows.push_back(h(prefix,
                text(std::string{cmd.label},
                     sel ? fg_bold(fg) : fg_of(muted)) | clip,
                spacer(),
                text(std::string{cmd.description}, fg_dim(muted)) | clip
            ).build());
        }
    }
    return wrap_picker({.pct_w = 70, .min_w = 50, .max_w = 110,
                        .accent = highlight, .title = " Command Palette "},
                       std::move(rows));
}

Element mention_palette(const Model& m) {
    auto* o = mention_opened(m.ui.mention_palette);
    if (!o) return nothing();

    std::vector<Element> rows;
    rows.push_back(h(text("@", fg_bold(info)),
        text(o->query.empty() ? " type to filter files…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);

    auto matches = filter_files(o->files, o->query);
    if (o->files.empty()) {
        rows.push_back(text("  workspace empty (or no readable files)", fg_italic(muted)));
    } else if (matches.empty()) {
        rows.push_back(text("  no matches", fg_italic(muted)));
    } else {
        int total = static_cast<int>(matches.size());
        int top = 0;
        if (o->index >= kVisible) top = o->index - kVisible + 1;
        if (top + kVisible > total) top = std::max(0, total - kVisible);
        for (int i = top; i < std::min(top + kVisible, total); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            bool sel = i == o->index;
            auto prefix = sel ? text("› ", fg_bold(info)) : text("  ");
            // text(...) | clip uses TextWrap::TruncateEnd — maya
            // measures the column it gives the text and returns a
            // truncated-with-ellipsis single line if the column is
            // narrower than the natural content. Combined with the
            // outer percent/min/max sizing the rows naturally adapt
            // to terminal width without moha doing any column math.
            rows.push_back(h(prefix,
                text(std::string{name},
                     sel ? fg_bold(fg) : fg_of(fg)) | clip,
                spacer(),
                text(parent_segment(dir), fg_dim(muted)) | clip
            ).build());
        }
        if (total > kVisible) {
            rows.push_back(text(
                "  " + std::to_string(o->index + 1) + "/" + std::to_string(total),
                fg_dim(muted)));
        }
    }
    return wrap_picker({.pct_w = 85, .min_w = 50, .max_w = 130,
                        .accent = info, .title = " Mention File "},
                       std::move(rows));
}

Element symbol_palette(const Model& m) {
    auto* o = symbol_palette_opened(m.ui.symbol_palette);
    if (!o) return nothing();

    std::vector<Element> rows;
    rows.push_back(h(text("#", fg_bold(highlight)),
        text(o->query.empty() ? " type to filter symbols…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);

    auto matches = filter_symbols(o->entries, o->query);
    if (o->entries.empty()) {
        rows.push_back(text("  no symbols indexed", fg_italic(muted)));
    } else if (matches.empty()) {
        rows.push_back(text("  no matches", fg_italic(muted)));
    } else {
        int total = static_cast<int>(matches.size());
        int top = 0;
        if (o->index >= kVisible) top = o->index - kVisible + 1;
        if (top + kVisible > total) top = std::max(0, total - kVisible);
        for (int i = top; i < std::min(top + kVisible, total); ++i) {
            const auto& sym = o->entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            bool sel = i == o->index;
            auto prefix = sel ? text("› ", fg_bold(highlight)) : text("  ");
            std::string locus = std::string{fname} + ":"
                              + std::to_string(sym.line_number);
            rows.push_back(h(prefix,
                text(sym.name, sel ? fg_bold(fg) : fg_of(fg)) | clip,
                text("  "),
                text(locus, fg_dim(muted)) | clip,
                spacer(),
                text(parent_segment(dir), fg_dim(muted)) | clip
            ).build());
        }
        if (total > kVisible) {
            rows.push_back(text(
                "  " + std::to_string(o->index + 1) + "/" + std::to_string(total),
                fg_dim(muted)));
        }
    }
    return wrap_picker({.pct_w = 85, .min_w = 60, .max_w = 140,
                        .accent = highlight, .title = " Symbol "},
                       std::move(rows));
}

Element todo_modal(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return nothing();

    std::vector<Element> rows;

    if (m.ui.todo.items.empty()) {
        rows.push_back(text("  No tasks yet.", fg_italic(muted)));
        rows.push_back(text("  The agent will create tasks as it works.", fg_dim(muted)));
    } else {
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
        rows.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        rows.push_back(text(""));
        rows.push_back(h(
            text("  " + std::to_string(done_count) + "/" + std::to_string(total),
                 fg_bold(done_count == total ? success : info)),
            text(" completed", fg_dim(muted))
        ).build());
    }

    rows.push_back(text(""));
    rows.push_back(h(
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return wrap_picker({.pct_w = 60, .min_w = 45, .max_w = 90,
                        .accent = info, .title = " Plan "},
                       std::move(rows));
}

} // namespace moha::ui
