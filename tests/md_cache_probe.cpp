// md_cache_probe — per-frame cost of StreamingMarkdown::build under long
// streaming shapes, verifying the memo layers hold (amortised O(1)/frame).
// Streams each shape token-ish chunk by chunk with reveal pacing live, and
// reports per-frame build+render_tree cost over the stream's life, split
// into quartiles of stream progress (a flat profile = caches hold; a
// growing profile = some O(N) path escaped the memos).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
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
using clk = std::chrono::steady_clock;

static constexpr int kWidth = 100;
static constexpr int kTermH = 40;

struct ShapeGen { const char* name; std::string body; };

static std::vector<ShapeGen> shapes() {
    std::vector<ShapeGen> v;
    { // long LOOSE list — blanks between every item (the new cohesive path)
        std::string b;
        for (int i = 0; i < 400; ++i) {
            b += "- loose item number " + std::to_string(i)
               + " with a bit of text\n\n";
        }
        b += "after paragraph\n";
        v.push_back({"loose_list_400", std::move(b)});
    }
    { // long TIGHT list — the chunker's home turf (regression check)
        std::string b;
        for (int i = 0; i < 400; ++i)
            b += "- tight item number " + std::to_string(i)
               + " with a bit of text\n";
        b += "\nafter paragraph\n";
        v.push_back({"tight_list_400", std::move(b)});
    }
    { // long quote-nested code fence (new dequote parity scan runs on
      // marker-only live lines)
        std::string b = "> intro\n> ```\n";
        for (int i = 0; i < 300; ++i)
            b += "> quoted code line " + std::to_string(i) + "\n";
        b += "> ```\n\nafter\n";
        v.push_back({"quote_fence_300", std::move(b)});
    }
    { // many paragraphs — commit path / frozen prefix
        std::string b;
        for (int i = 0; i < 200; ++i)
            b += "paragraph number " + std::to_string(i)
               + " has some words in it to wrap around maybe.\n\n";
        v.push_back({"paras_200", std::move(b)});
    }
    { // long table
        std::string b = "| Col A | Col B | Col C |\n|---|---|---|\n";
        for (int i = 0; i < 300; ++i)
            b += "| row " + std::to_string(i) + " | data | more |\n";
        b += "\nafter\n";
        v.push_back({"table_300", std::move(b)});
    }
    { // many link-ref definitions (new zero-row path)
        std::string b = "See [a][1] and [b][2].\n\n";
        for (int i = 0; i < 200; ++i)
            b += "[" + std::to_string(i) + "]: https://example.com/"
               + std::to_string(i) + "\n";
        b += "\nafter\n";
        v.push_back({"link_refs_200", std::move(b)});
    }
    return v;
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    bool bad = false;
    for (auto& sh : shapes()) {
        if (only && std::strcmp(only, sh.name)) continue;
        StreamingMarkdown md;
        md.set_reveal_fx(true);
        md.set_reveal_pacing(2000.0, 0.3);  // fast cursor: don't bottleneck on reveal
        md.set_live(true);

        StylePool pool;
        std::vector<layout::LayoutNode> nodes;

        std::vector<double> frame_us;
        std::size_t fed = 0;
        const std::size_t chunk = 24;   // ~token-sized
        while (fed < sh.body.size()) {
            std::size_t n = std::min(chunk, sh.body.size() - fed);
            md.append(std::string_view{sh.body}.substr(fed, n));
            fed += n;
            maya::testing::advance_anim_clock_ms(16);
            auto t0 = clk::now();
            {
                RenderContext ctx{kWidth, kTermH, render_generation(), true};
                RenderContextGuard guard(ctx);
                Canvas c(kWidth, 6000, &pool);
                c.clear();
                render_tree(md.build(), c, pool, theme::dark, nodes, true);
            }
            auto t1 = clk::now();
            frame_us.push_back(
                std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        // Per-quartile MEDIAN, not mean. A mean is dragged up by sporadic
        // scheduler-preemption spikes (see the multi-millisecond `worst`
        // values) that have nothing to do with cache behaviour — under a
        // loaded `ctest -j` those outliers can push a small-q1 / spiky-q4
        // ratio over the escape threshold and flake the probe. The median
        // tracks the TREND (an O(N) leak shifts the whole distribution) while
        // rejecting isolated stalls, so it stays honest under contention.
        auto qmed = [&](std::size_t a, std::size_t b) {
            std::vector<double> w;
            for (std::size_t i = a; i < b && i < frame_us.size(); ++i)
                w.push_back(frame_us[i]);
            if (w.empty()) return 0.0;
            std::sort(w.begin(), w.end());
            return w[w.size() / 2];
        };
        std::size_t N = frame_us.size();
        double q1 = qmed(0, N/4), q2 = qmed(N/4, N/2),
               q3 = qmed(N/2, 3*N/4), q4 = qmed(3*N/4, N);
        double worst = 0; for (double f : frame_us) if (f > worst) worst = f;
        // growth ratio: last quartile vs first (>3x and >1ms = escaped memo).
        // Both conditions must hold: the median rejects contention outliers,
        // and the 1ms floor keeps sub-millisecond jitter from tripping a
        // benign ratio.
        double ratio = q1 > 0 ? q4 / q1 : 0.0;
        bool flag = q4 > 1000.0 && ratio > 3.0;
        if (flag) bad = true;
        std::printf("%-18s frames=%4zu  q1=%7.0fus q2=%7.0fus q3=%7.0fus q4=%7.0fus  worst=%7.0fus  growth=%.1fx%s\n",
                    sh.name, N, q1, q2, q3, q4, worst, ratio,
                    flag ? "  <-- O(N) ESCAPE" : "");
    }
    if (!bad) std::puts("CACHES HOLD (flat per-frame profile)");
    return bad ? 1 : 0;
}
