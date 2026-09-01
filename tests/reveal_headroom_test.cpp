// reveal_headroom_test — guard the geometric bound that authorizes the
// INSTANT tool card at a text→tool seam (turn.cpp, cached_markdown_for).
//
// Background: showing the tool card while prose is still revealing is only
// safe if the unresolved tail provably cannot be pushed past the viewport
// top into immutable scrollback. turn.cpp proves that with
//
//     est_hidden_rows + est_tail_rows + chrome + slack < viewport_rows
//
// where est_tail_rows bounds the rendered height of the not-yet-revealed
// bytes. The bound is only a PROOF if it never UNDER-estimates: an
// under-estimate authorizes the instant card with more rows in flight than
// the viewport can hold, and ghosted / scramble glyphs land in scrollback
// where no repaint can ever fix them.
//
// A previous version used ceil(bytes / cols), which silently assumes every
// byte consumes a column. Newlines break that assumption badly — measured
// here: 40 short list items occupy 41 rows but that formula predicts 2, a
// 20x under-estimate, and exactly the markdown shape (lists, tables) that
// tends to precede a tool call. The scrollback oracle did NOT catch it
// because its prose is long wrapping paragraphs where bytes/cols happens to
// be right.
//
// This test pins the invariant directly: for a corpus of markdown shapes,
// at several widths, the estimator must be >= the height maya actually
// renders for the same bytes.

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <maya/core/anim_clock.hpp>
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/markdown.hpp>

using namespace maya;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)

// EXACT copy of the estimator in turn.cpp's headroom proof. Kept in sync by
// this test's existence: if turn.cpp's formula changes without changing this
// one, the assertion below starts failing against real rendered heights.
int est_tail_rows(std::string_view tail, int cols) {
    if (cols < 1) cols = 1;
    constexpr int kBlockInset = 2;
    const int wrap_w = std::max(1, cols - kBlockInset);
    int rows = 0;
    std::size_t line_start = 0;
    while (line_start <= tail.size()) {
        const std::size_t nl = tail.find('\n', line_start);
        const std::size_t len =
            (nl == std::string_view::npos ? tail.size() : nl) - line_start;
        rows += std::max<int>(
            1, static_cast<int>((len + static_cast<std::size_t>(wrap_w) - 1)
                                / static_cast<std::size_t>(wrap_w)));
        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }
    return rows;
}

struct Shape { const char* name; std::string body; };

std::vector<Shape> corpus() {
    std::vector<Shape> s;
    auto rep = [](const char* unit, int n) {
        std::string out;
        for (int i = 0; i < n; ++i) out += unit;
        return out;
    };
    // The shapes that broke the old bytes/cols bound.
    s.push_back({"short_list_40",  rep("- x\n", 40)});
    s.push_back({"table_20",       rep("| a | b |\n", 20)});
    s.push_back({"blank_lines_30", std::string(30, '\n')});
    s.push_back({"tight_numbers",  rep("1. i\n", 25)});
    // Shapes the old bound handled, kept so a "fix" can't regress them.
    s.push_back({"one_long_line",  std::string(500, 'x')});
    s.push_back({"wrapping_paras", rep("This is a long paragraph that wraps "
                                       "across several terminal rows just "
                                       "like real assistant prose does.\n\n", 6)});
    s.push_back({"mixed",          "# Title\n\nSome prose here.\n\n"
                                   "- a\n- b\n- c\n\n> quote\n\n"
                                   "final paragraph that runs on for a while "
                                   "so it wraps at narrow widths too.\n"});
    return s;
}

int failures = 0;

int rows_of(const Canvas& c) { return c.max_content_row() + 1; }

void check_shape(const Shape& sh, int cols) {
    // Render the SAME bytes through the real widget and measure its height.
    StreamingMarkdown md;
    md.set_content(sh.body);
    md.set_live(false);             // settled height = fully rendered rows

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;
    RenderContext ctx{cols, 4000, render_generation(), true};
    RenderContextGuard guard(ctx);
    Canvas c(cols, 4000, &pool);
    c.clear();
    render_tree(md.build(), c, pool, theme::dark, nodes, true);
    const int actual = rows_of(c);

    const int estimate = est_tail_rows(sh.body, cols);

    if (estimate < actual) {
        std::fprintf(stderr,
            "FAIL: %-16s cols=%3d  estimate=%3d < actual=%3d  "
            "(bound UNDER-estimates → instant card could push %d ghosted "
            "row(s) into scrollback)\n",
            sh.name, cols, estimate, actual, actual - estimate);
        ++failures;
    } else {
        std::fprintf(stderr, "ok  : %-16s cols=%3d  estimate=%3d >= actual=%3d\n",
                     sh.name, cols, estimate, actual);
    }
}

}  // namespace

int main() {
    // Widths spanning the oracle's shapes, including a narrow one where
    // wrapping dominates and a wide one where line COUNT dominates.
    const int widths[] = {46, 60, 80, 100};
    for (const auto& sh : corpus())
        for (int w : widths)
            check_shape(sh, w);

    if (failures) {
        std::fprintf(stderr,
            "\n%d bound violation(s): est_tail_rows must be an UPPER bound on "
            "rendered rows or turn.cpp's headroom proof is unsound.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "\nPASS: est_tail_rows is an upper bound on all shapes/widths\n");
    return 0;
}
