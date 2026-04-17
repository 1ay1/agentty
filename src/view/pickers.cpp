#include "moha/view/pickers.hpp"

#include <vector>

#include "moha/view/helpers.hpp"
#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element model_picker(const Model& m) {
    if (!m.model_picker.open) return text("");
    std::vector<Element> rows;
    rows.push_back(text("Select model", fg_bold(fg)));
    rows.push_back(text(""));
    int i = 0;
    for (const auto& mi : m.available_models) {
        bool sel    = i == m.model_picker.index;
        bool active = mi.id == m.model_id;
        auto prefix = sel ? text("\u203A ", fg_bold(accent)) : text("  ");
        auto star   = mi.favorite ? text("\u2605 ", fg_of(warn)) : text("  ");
        auto active_mark = active ? text(" (active)", fg_of(success)) : text("");
        rows.push_back(h(prefix, star,
            text(mi.display_name,
                 sel ? fg_bold(fg) : fg_of(muted)),
            active_mark).build());
        ++i;
    }
    rows.push_back(text(""));
    rows.push_back(text("\u2191\u2193 move  Enter select  F favorite  Esc close",
                        fg_dim(muted)));
    auto content = (v(std::move(rows)) | padding(1, 2) | width(50));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(accent)
            | btext(" Models ")).build();
}

Element thread_list(const Model& m) {
    if (!m.thread_list.open) return text("");
    std::vector<Element> rows;
    rows.push_back(text("Recent threads", fg_bold(fg)));
    rows.push_back(text(""));
    if (m.threads.empty())
        rows.push_back(text("No threads yet.", fg_italic(muted)));
    int i = 0;
    for (const auto& t : m.threads) {
        bool sel = i == m.thread_list.index;
        auto prefix = sel ? text("\u203A ", fg_bold(info)) : text("  ");
        rows.push_back(h(prefix,
            text(t.title.empty() ? "(untitled)" : t.title,
                 sel ? fg_of(fg) : fg_of(muted)),
            spacer(),
            text(timestamp_hh_mm(t.updated_at), fg_dim(muted))
        ).build());
        if (++i > 15) break;
    }
    rows.push_back(text(""));
    rows.push_back(text("\u2191\u2193 move  Enter open  N new  Esc close",
                        fg_dim(muted)));
    auto content = (v(std::move(rows)) | padding(1, 2) | width(60));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(info)
            | btext(" Threads ")).build();
}

Element command_palette(const Model& m) {
    if (!m.command_palette.open) return text("");
    static const std::pair<const char*, const char*> kCmds[] = {
        {"New thread",         "Start a fresh conversation"},
        {"Review changes",     "Open diff review pane"},
        {"Accept all changes", "Apply every pending hunk"},
        {"Reject all changes", "Discard every pending hunk"},
        {"Cycle profile",      "Write \u2192 Ask \u2192 Minimal"},
        {"Open model picker",  ""},
        {"Open threads",       ""},
        {"Quit",               "Exit moha"},
    };
    std::vector<Element> rows;
    rows.push_back(h(text("\u203A ", fg_bold(highlight)),
        text(m.command_palette.query.empty() ? "(type to filter)"
                                              : m.command_palette.query,
             m.command_palette.query.empty() ? fg_of(muted) : fg_of(fg))
    ).build());
    rows.push_back(sep);
    int i = 0;
    for (const auto& [name, desc] : kCmds) {
        if (!m.command_palette.query.empty()
            && std::string_view{name}.find(m.command_palette.query) == std::string_view::npos)
            continue;
        bool sel = i == m.command_palette.index;
        auto prefix = sel ? text("\u203A ", fg_bold(highlight)) : text("  ");
        rows.push_back(h(prefix,
            text(name, sel ? fg_bold(fg) : fg_of(muted)),
            spacer(),
            text(desc, fg_dim(muted))).build());
        ++i;
    }
    auto content = (v(std::move(rows)) | padding(1, 2) | width(70));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(highlight)
            | btext(" Command ")).build();
}

} // namespace moha::ui
