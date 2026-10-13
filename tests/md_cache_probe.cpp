// md_cache_probe — per-frame cost of StreamingMarkdown::build under long
// streaming shapes, verifying the memo layers hold (amortised O(1)/frame).
// Streams each shape token-ish chunk by chunk with reveal pacing live, and
// reports per-frame build+render_tree cost over the stream's life, split
// into quartiles of stream progress (a flat profile = caches hold; a
// growing profile = some O(N) path escaped the memos).
#include <chrono>
#include <ctime>     // clock_gettime, CLOCK_THREAD_CPUTIME_ID
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

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)

// Per-frame cost is measured in THREAD CPU TIME, not wall time. Wall time also
// counts the time this thread spent descheduled, which on a loaded box grows
// with however many other tests are running -- that turned a per-frame cost
// ratio into a measure of machine load. CPU time only advances while this
// thread is on-core. (It is not a complete fix on its own: memory-bandwidth
// contention still inflates real CPU work, which is why the ctest entry is
// also RUN_SERIAL and the budgets below carry headroom.)
//
// CLOCK_THREAD_CPUTIME_ID is POSIX; elsewhere fall back to wall time, since a
// fallback that still works beats one that doesn't compile.
[[nodiscard]] double cpu_now_us() noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return static_cast<double>(ts.tv_sec) * 1e6
             + static_cast<double>(ts.tv_nsec) / 1e3;
#endif
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

static constexpr int kWidth = 100;
static constexpr int kTermH = 40;

// `budget` is the max q4/q1 growth this shape may show before it counts as an
// escaped memo. It is PER SHAPE because the shapes do not all have a flat
// honest profile: a growing quote-nested fence really does cost more per frame
// as it grows, measuring ~2.9x on an idle box, while the list and paragraph
// shapes sit at ~1.1-1.2x. The probe used one global 3.0x for everything,
// which was wrong in both directions at once -- quote_fence_300 had 0.1x of
// headroom and failed whenever the box was busy (read as flaky), while
// paras_200 was allowed to nearly TRIPLE before anyone noticed.
//
// Calibrated by injecting a synthetic O(N) cost into the timed region:
// a moderate leak lands at 3.6x on the flat shapes and a bad one at 5-6x, so
// these budgets catch both while clearing the measured idle baseline (shown
// per shape below) with room for a slow or loaded machine.
struct ShapeGen { const char* name; std::string body; double budget; };

static std::vector<ShapeGen> shapes() {
    std::vector<ShapeGen> v;
    { // long LOOSE list — blanks between every item (the new cohesive path)
        std::string b;
        for (int i = 0; i < 400; ++i) {
            b += "- loose item number " + std::to_string(i)
               + " with a bit of text\n\n";
        }
        b += "after paragraph\n";
        v.push_back({"loose_list_400", std::move(b), 2.5});  // idle ~1.2x
    }
    { // long TIGHT list — the chunker's home turf (regression check)
        std::string b;
        for (int i = 0; i < 400; ++i)
            b += "- tight item number " + std::to_string(i)
               + " with a bit of text\n";
        b += "\nafter paragraph\n";
        v.push_back({"tight_list_400", std::move(b), 2.5});  // idle ~1.2x
    }
    { // long quote-nested code fence (new dequote parity scan runs on
      // marker-only live lines)
        std::string b = "> intro\n> ```\n";
        for (int i = 0; i < 300; ++i)
            b += "> quoted code line " + std::to_string(i) + "\n";
        b += "> ```\n\nafter\n";
        // The one genuinely growing shape: the dequote parity scan re-runs on
        // marker-only live lines, so per-frame cost rises with the fence.
        v.push_back({"quote_fence_300", std::move(b), 4.0});  // idle ~2.9x
    }
    { // many paragraphs — commit path / frozen prefix
        std::string b;
        for (int i = 0; i < 200; ++i)
            b += "paragraph number " + std::to_string(i)
               + " has some words in it to wrap around maybe.\n\n";
        v.push_back({"paras_200", std::move(b), 2.5});  // idle ~1.1x
    }
    { // long table
        std::string b = "| Col A | Col B | Col C |\n|---|---|---|\n";
        for (int i = 0; i < 300; ++i)
            b += "| row " + std::to_string(i) + " | data | more |\n";
        b += "\nafter\n";
        v.push_back({"table_300", std::move(b), 2.5});  // idle ~1.1x
    }
    { // many link-ref definitions (new zero-row path)
        std::string b = "See [a][1] and [b][2].\n\n";
        for (int i = 0; i < 200; ++i)
            b += "[" + std::to_string(i) + "]: https://example.com/"
               + std::to_string(i) + "\n";
        b += "\nafter\n";
        v.push_back({"link_refs_200", std::move(b), 3.0});  // idle ~1.9-2.2x
    }
    return v;
}

}  // namespace (fold)

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
            const double t0 = cpu_now_us();
            {
                RenderContext ctx{kWidth, kTermH, render_generation(), true};
                RenderContextGuard guard(ctx);
                Canvas c(kWidth, 6000, &pool);
                c.clear();
                render_tree(md.build(), c, pool, theme::native, nodes, true);
            }
            const double t1 = cpu_now_us();
            frame_us.push_back(t1 - t0);
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
        // growth ratio: last quartile vs first. Both conditions must hold:
        // the median rejects contention outliers, and the 1ms floor keeps
        // sub-millisecond jitter from tripping a benign ratio. The ceiling is
        // this shape's own budget -- see ShapeGen.
        double ratio = q1 > 0 ? q4 / q1 : 0.0;
        bool flag = q4 > 1000.0 && ratio > sh.budget;
        if (flag) bad = true;
        std::printf("%-18s frames=%4zu  q1=%7.0fus q2=%7.0fus q3=%7.0fus q4=%7.0fus  worst=%7.0fus  growth=%.1fx/%.1fx%s\n",
                    sh.name, N, q1, q2, q3, q4, worst, ratio, sh.budget,
                    flag ? "  <-- O(N) ESCAPE" : "");
    }
    if (!bad) std::puts("CACHES HOLD (flat per-frame profile)");
    return bad ? 1 : 0;
}
