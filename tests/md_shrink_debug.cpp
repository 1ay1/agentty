// md_shrink_debug — replay one shape byte-by-byte, dumping rendered rows at
// the shrink frame and the frame before it.
#include <cstdio>
#include <cstring>
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
static constexpr int kWidth = 80;

static std::string row_text(const Canvas& c, int y) {
    std::string out;
    for (int x = 0; x < kWidth; ++x) {
        char32_t ch = c.get(x, y).character;
        if (ch == 0) ch = U' ';
        if (ch < 0x80) out.push_back(static_cast<char>(ch));
        else out.push_back('#');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

int main(int argc, char** argv) {
    std::string body;
    const char* which = argc > 1 ? argv[1] : "link_ref";
    if (!std::strcmp(which, "link_ref"))
        body = "See [the docs][d] for more.\n\n[d]: https://example.com\n\nafter\n";
    else if (!std::strcmp(which, "loose_list"))
        body = "- first item\n\n- second item\n\n- third item\n\nafter\n";
    else if (!std::strcmp(which, "nested_list"))
        body = "- outer one\n  - inner a\n  - inner b\n- outer two\n\nafter\n";
    else if (!std::strcmp(which, "html_block"))
        body = "<div>\nraw html body\n</div>\n\nafter\n";
    else if (!std::strcmp(which, "quote_code"))
        body = "> quoted\n> ```\n> code in quote\n> ```\n\nafter\n";

    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(90.0, 0.3);
    md.set_live(true);

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;

    std::vector<std::string> prev_dump;
    int prev_rows = 0;
    for (std::size_t fed = 0; fed < body.size(); ++fed) {
        md.append(std::string_view{body}.substr(fed, 1));
        maya::testing::advance_anim_clock_ms(16);
        RenderContext ctx{kWidth, 40, render_generation(), true};
        RenderContextGuard guard(ctx);
        Canvas c(kWidth, 2000, &pool);
        c.clear();
        render_tree(md.build(), c, pool, theme::dark, nodes, true);
        int rows = c.max_content_row() + 1;
        std::vector<std::string> dump;
        for (int y = 0; y < rows; ++y) dump.push_back(row_text(c, y));
        if (rows < prev_rows) {
            std::printf("=== SHRINK at byte %zu (fed '%c', 0x%02x): %d -> %d rows\n",
                        fed + 1, body[fed] == '\n' ? '$' : body[fed],
                        (unsigned)body[fed], prev_rows, rows);
            std::printf("--- before:\n");
            for (auto& r : prev_dump) std::printf("  |%s|\n", r.c_str());
            std::printf("--- after:\n");
            for (auto& r : dump) std::printf("  |%s|\n", r.c_str());
        }
        prev_rows = rows;
        prev_dump = std::move(dump);
    }
    return 0;
}
