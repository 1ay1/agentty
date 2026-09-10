// slice_width_probe — where does a split slice lose a column?
//
// Symptom: on the Session tab between roughly 96 and 124 columns, a value
// clips mid-string ("6.0s" renders as "6") even though the sheet's own
// measurement says the row has ~23 columns to spare.
//
// Two widths are computed for the same cell and they disagree:
//   render_slice(rows, ..., inner)   lays the row out for `inner`
//   cell.layout.width = fixed(inner) gives the cell `inner`
//
// Those look identical, which is why reading the code did not find it. This
// prints what each stage ACTUALLY produces, so the discrepancy has to show
// itself rather than be reasoned about.

#include <maya/widget/stat_sheet.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace maya;

namespace {

// The Session tab's shape: a hero, a table, a donut, a second table. The
// donut is what forces the split, and the second table is what clips.
Element session_like() {
    StatSheet s;
    s.indent(1);
    s.reserve_right(7);
    s.columns(46, 2);

    s.hero("8 turns", "none ended in an error");
    s.blank();
    s.heading("Activity");
    s.entry({.label = "You asked",     .value = "8",  .share = 0.8});
    s.entry({.label = "Agent replied", .value = "8",  .share = 0.8});
    s.entry({.label = "Tool calls",    .value = "10", .share = 1.0});
    s.blank();

    s.heading("where the time went");
    StatDonut d;
    d.segments = {{"wait", 8, Color::yellow()},
                  {"gen", 81, Color::magenta()},
                  {"tools", 11, Color::green()}};
    d.center = "1m14s";
    s.donut(std::move(d));

    s.blank();
    s.heading("Time");
    s.entry({.label = "Waiting",      .value = "6.0s", .share = 0.10});
    s.entry({.label = "Generating",   .value = "59s",  .share = 1.00});
    s.entry({.label = "Tools",        .value = "8.2s", .share = 0.14});
    s.entry({.label = "Average turn", .value = "8.2s", .share = 0.14});
    return s.build();
}

// Flatten one row of the rendered sheet and report what survived.
std::string row_containing(const Element& el, int w, const char* needle) {
    StylePool pool;
    Canvas canvas(w, 60, &pool);
    render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);

    for (int y = 0; y < 60; ++y) {
        std::string line;
        for (int x = 0; x < w; ++x) {
            const char32_t c = canvas.get(x, y).character;
            const char32_t ch = c ? c : U' ';
            if (ch < 0x80) line += static_cast<char>(ch);
            else if (ch < 0x800) {
                line += static_cast<char>(0xC0 | (ch >> 6));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                line += static_cast<char>(0xE0 | (ch >> 12));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                line += static_cast<char>(0xF0 | (ch >> 18));
                line += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        if (line.find(needle) != std::string::npos) {
            // Trim trailing blanks so the row's true right edge is visible.
            auto end = line.find_last_not_of(' ');
            return end == std::string::npos ? line : line.substr(0, end + 1);
        }
    }
    return "<not found>";
}

}  // namespace

int main() {
    std::printf("The sheet is built at WIDTH; the split gives each slice\n"
                "inner = (WIDTH - reserve(7) - gap(3)) / 2.\n\n");
    std::printf("%-7s %-7s  %s\n", "width", "inner", "the Waiting row, as painted");

    for (int w = 88; w <= 136; w += 4) {
        const int inner = (w - 7 - 3) / 2;
        const std::string row = row_containing(session_like(), w, "Waiting");
        // Where does the row's ink actually end, and did the value survive?
        const bool has_value = row.find("6.0s") != std::string::npos;
        std::printf("%-7d %-7d  ends@%-4zu %s  %s\n",
                    w, inner, row.size(),
                    has_value ? "value OK  " : "VALUE CLIPPED",
                    row.size() > 40 ? row.substr(row.size() - 34).c_str() : row.c_str());
    }
    return 0;
}
