// md_shape_sweep — hunt for streaming height-shrink (composer flicker) across
// a corpus of tricky markdown shapes. Any frame where the rendered height
// DECREASES while live is a composer bounce: the widget got taller then
// shorter, dragging the composer up and back down. Streams each shape at
// several chunk sizes; reports shrink events with the offending byte window.
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

static constexpr int kWidth = 80;
static constexpr int kTermH = 40;

struct Shape { const char* name; std::string body; };

static std::vector<Shape> corpus() {
    std::vector<Shape> s;
    s.push_back({"setext_h1", "Title line\n===\n\nbody text\n"});
    s.push_back({"setext_h2", "Sub title\n---\n\nbody text\n"});
    s.push_back({"hr_after_para", "some paragraph\n\n---\n\nmore text\n"});
    s.push_back({"loose_list",
        "- first item\n\n- second item\n\n- third item\n\nafter\n"});
    s.push_back({"nested_list",
        "- outer one\n  - inner a\n  - inner b\n- outer two\n\nafter\n"});
    s.push_back({"ordered_start", "3. third\n4. fourth\n5. fifth\n\nafter\n"});
    s.push_back({"task_list", "- [ ] todo one\n- [x] done two\n\nafter\n"});
    s.push_back({"quote_then_alert",
        "> [!NOTE]\n> this becomes an alert callout\n> with two rows\n\nafter\n"});
    s.push_back({"quote_lazy",
        "> quoted first line\nlazy continuation line\n\nafter\n"});
    s.push_back({"fence_tilde", "~~~py\nprint('hi')\nprint('bye')\n~~~\n\nafter\n"});
    s.push_back({"fence_nested_ticks",
        "````md\ninner ```js\ncode\n```\nstill inside\n````\n\nafter\n"});
    s.push_back({"fence_no_lang_close_immediately", "```\n```\n\nafter\n"});
    s.push_back({"indented_code", "    indented code line one\n    line two\n\nafter\n"});
    s.push_back({"table_wrapcell",
        "| A | B |\n|---|---|\n| a very long cell body that will wrap in eighty columns for sure yes | b |\n| c | d |\n\nafter\n"});
    s.push_back({"table_then_para",
        "| A | B |\n|---|---|\n| 1 | 2 |\n\nplain paragraph after the table\n"});
    s.push_back({"heading_trailing_hashes", "## closed heading ##\n\nbody\n"});
    s.push_back({"html_block", "<div>\nraw html body\n</div>\n\nafter\n"});
    s.push_back({"link_ref",
        "See [the docs][d] for more.\n\n[d]: https://example.com\n\nafter\n"});
    s.push_back({"long_wrap_line",
        std::string(300, 'x') + "\n\nafter\n"});
    s.push_back({"emphasis_spill",
        "some *emphasis that spans\nmultiple source lines* in a paragraph\n\nafter\n"});
    s.push_back({"list_then_fence",
        "- item one\n- item two\n\n```c\nint x;\n```\n\nafter\n"});
    s.push_back({"quote_code",
        "> quoted\n> ```\n> code in quote\n> ```\n\nafter\n"});
    s.push_back({"hard_break", "line one  \nline two\\\nline three\n\nafter\n"});
    s.push_back({"heading_h1_h6", "# one\n\n###### six\n\nbody\n"});
    s.push_back({"para_then_setext_trap",
        "could be paragraph\ncontinued here\n---\n\nafter\n"});
    s.push_back({"empty_lines_run", "para\n\n\n\n\npara two\n"});
    s.push_back({"footnote_like", "text with [^1] mark\n\n[^1]: the footnote body\n\nafter\n"});
    s.push_back({"bold_heading", "## **bold** heading with `code`\n\nbody\n"});
    s.push_back({"table_alignment",
        "| L | C | R |\n|:--|:-:|--:|\n| a | b | c |\n\nafter\n"});
    s.push_back({"strikethrough", "~~struck~~ text and more\n\nafter\n"});
    return s;
}

static int rows_of(const Canvas& c) { return c.max_content_row() + 1; }

int main() {
    StylePool pool_dummy;
    int total_fail = 0;
    const int chunk_sizes[] = {1, 3, 7};

    for (const auto& sh : corpus()) {
        for (int chunk : chunk_sizes) {
            StreamingMarkdown md;
            md.set_reveal_fx(true);
            md.set_reveal_pacing(90.0, 0.3);
            md.set_live(true);

            StylePool pool;
            std::vector<layout::LayoutNode> nodes;
            auto paint = [&]() -> int {
                RenderContext ctx{kWidth, kTermH, render_generation(), true};
                RenderContextGuard guard(ctx);
                Canvas c(kWidth, 2000, &pool);
                c.clear();
                render_tree(md.build(), c, pool, theme::dark, nodes, true);
                return rows_of(c);
            };

            int prev = 0;
            int shrinks = 0;
            std::size_t worst_at = 0;
            int worst_delta = 0;
            std::size_t fed = 0;
            while (fed < sh.body.size()) {
                std::size_t n = std::min<std::size_t>(
                    static_cast<std::size_t>(chunk), sh.body.size() - fed);
                md.append(std::string_view{sh.body}.substr(fed, n));
                fed += n;
                maya::testing::advance_anim_clock_ms(16);
                int r = paint();
                if (r < prev) {
                    ++shrinks;
                    if (prev - r > worst_delta) { worst_delta = prev - r; worst_at = fed; }
                }
                prev = r;
            }
            // drain
            int guard = 0;
            while ((md.is_live() || md.is_finalizing()) && guard++ < 400) {
                md.request_finalize(200);
                maya::testing::advance_anim_clock_ms(16);
                int r = paint();
                if (r < prev) {
                    ++shrinks;
                    if (prev - r > worst_delta) { worst_delta = prev - r; worst_at = fed; }
                }
                prev = r;
            }
            md.finish();
            maya::testing::advance_anim_clock_ms(16);
            (void)paint();

            if (shrinks > 0) {
                ++total_fail;
                std::printf("SHRINK %-28s chunk=%d events=%d worst=-%d rows near byte %zu\n",
                            sh.name, chunk, shrinks, worst_delta, worst_at);
                // print the byte window for diagnosis
                std::size_t a = worst_at > 24 ? worst_at - 24 : 0;
                std::size_t b = std::min(sh.body.size(), worst_at + 8);
                std::string win{sh.body.substr(a, b - a)};
                for (auto& ch : win) if (ch == '\n') ch = '$';
                std::printf("        window: \"%s\"\n", win.c_str());
            }
        }
    }
    if (total_fail == 0) std::puts("ALL SHAPES MONOTONIC");
    return total_fail == 0 ? 0 : 1;
}
