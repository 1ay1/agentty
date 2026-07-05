// reveal_smoothness_probe — quantify the streaming reveal's per-frame
// smoothness across BLOCK TYPES (prose / heading / code fence / table /
// list / blockquote).
//
// The user-facing goal: "the md streaming animation should feel the same
// across the whole turn — code blocks, tables and all". Mechanically that
// means the number of newly-visible content cells per frame should be
// bounded and roughly uniform regardless of which block type the reveal
// cursor is walking through. Two failure smells:
//
//   • BURST: a frame where a whole block's worth of content appears at
//     once (eager path outruns the reveal clip, or the clip is line-
//     granular over a very long line).
//   • STALL: many consecutive frames with zero new content while wire
//     bytes exist (cursor wedged behind an eager render that doesn't
//     consume the clip).
//
// Drives StreamingMarkdown EXACTLY as production does (turn.cpp):
// reveal_fx on, set_reveal_pacing(90, 0.3), append() per wire chunk,
// build() per frame under a sized RenderContext, virtual anim clock
// advanced 16 ms per frame. Renders each frame to a Canvas and counts
// non-blank cells. Reports per-section stats.

#include <algorithm>
#include <cstdint>
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

static constexpr int kWidth  = 100;
static constexpr int kTermH  = 40;
static constexpr int kFrameMs = 16;
// Wire feed: bytes per frame. 6 bytes / 16 ms ≈ 375 cps — a fast model.
static constexpr int kBytesPerFrame = 6;

struct Section { const char* name; std::string body; };

static std::vector<Section> make_doc() {
    std::vector<Section> s;
    s.push_back({"prose",
        "The quick brown fox jumps over the lazy dog while the markdown "
        "renderer paces every codepoint at a bounded readable speed so the "
        "reveal feels like typing rather than pasting. Another sentence "
        "follows to give the cursor a long uninterrupted paragraph to walk.\n\n"});
    s.push_back({"heading",
        "## Section heading streams in smoothly\n\n"});
    s.push_back({"code",
        "```cpp\nint main() {\n    auto x = compute_all_the_things();\n"
        "    for (int i = 0; i < 100; ++i) {\n        process(i, x);\n    }\n"
        "    return x > 0 ? 0 : 1;\n}\n```\n\n"});
    s.push_back({"table",
        "| Component | Latency | Throughput | Notes |\n"
        "|-----------|---------|------------|-------|\n"
        "| parser    | 0.2ms   | 480 MB/s   | SIMD-assisted scan |\n"
        "| layout    | 1.1ms   | n/a        | per-block memoised |\n"
        "| paint     | 0.4ms   | 60 fps     | cell-cache blits |\n"
        "| serialize | 0.3ms   | n/a        | row-diff only |\n\n"});
    s.push_back({"list",
        "- first item with enough words to wrap the terminal column nicely\n"
        "- second item continues the pattern of readable streaming text\n"
        "- third item keeps the cadence going for the reveal cursor\n"
        "- fourth item closes out the list body\n\n"});
    s.push_back({"quote",
        "> A blockquote that streams in with the same left-to-right glide\n"
        "> as everything around it, keeping the turn visually uniform.\n\n"});
    s.push_back({"prose2",
        "And a closing paragraph after all the structured blocks, so the "
        "settle ramp has ordinary prose to land on at the end of the turn.\n"});
    return s;
}

// Count CONTENT cells (alphanumeric/punct — excludes box-drawing border
// chrome, which legitimately appears whole when a block's frame renders).
static int content_cells(const Canvas& c) {
    int n = 0;
    const int mr = c.max_content_row();
    for (int y = 0; y <= mr; ++y)
        for (int x = 0; x < kWidth; ++x) {
            char32_t ch = c.get(x, y).character;
            if (ch == 0 || ch == U' ') continue;
            // Box-drawing / block elements = structural chrome.
            if (ch >= 0x2500 && ch <= 0x259F) continue;
            ++n;
        }
    return n;
}

static int content_rows(const Canvas& c) { return c.max_content_row() + 1; }

struct FrameStat { int section; int delta_cells; int rows; };

int main() {
    auto doc = make_doc();
    std::string full;
    std::vector<std::size_t> section_end;   // byte offset where section i ends
    for (auto& s : doc) { full += s.body; section_end.push_back(full.size()); }

    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(/*floor_cps=*/90.0, /*lead_secs=*/0.3);
    md.set_live(true);

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;

    auto paint = [&]() -> Canvas {
        RenderContext ctx{kWidth, kTermH, render_generation(), /*auto_height=*/true};
        RenderContextGuard guard(ctx);
        Canvas c(kWidth, 4000, &pool);
        c.clear();
        render_tree(md.build(), c, pool, theme::dark, nodes, /*auto_height=*/true);
        return c;
    };

    std::vector<FrameStat> stats;
    std::size_t fed = 0;
    int prev_cells = 0, prev_rows = 0;
    int shrink_events = 0;

    auto section_of = [&](std::size_t byte) -> int {
        for (std::size_t i = 0; i < section_end.size(); ++i)
            if (byte < section_end[i]) return static_cast<int>(i);
        return static_cast<int>(section_end.size()) - 1;
    };

    // Stream phase.
    while (fed < full.size()) {
        std::size_t n = std::min<std::size_t>(kBytesPerFrame, full.size() - fed);
        md.append(std::string_view{full}.substr(fed, n));
        fed += n;
        maya::testing::advance_anim_clock_ms(kFrameMs);
        Canvas c = paint();
        int cells = content_cells(c);
        int rows  = content_rows(c);
        if (rows < prev_rows) ++shrink_events;
        stats.push_back({section_of(fed > 0 ? fed - 1 : 0), cells - prev_cells, rows});
        prev_cells = cells; prev_rows = rows;
    }
    // Drain phase: production settle — arm the finalize ramp and keep
    // frames coming; the widget glides the backlog out and flips live_
    // off itself. finish() afterwards is the (shape-neutral) flush.
    int drain_frames = 0;
    const int drain_section = static_cast<int>(doc.size());   // pseudo-section
    while ((md.is_live() || md.is_finalizing()) && drain_frames < 600) {
        md.request_finalize(200);
        maya::testing::advance_anim_clock_ms(kFrameMs);
        Canvas c = paint();
        int cells = content_cells(c);
        int rows  = content_rows(c);
        if (rows < prev_rows) ++shrink_events;
        stats.push_back({drain_section, cells - prev_cells, rows});
        prev_cells = cells; prev_rows = rows;
        ++drain_frames;
    }
    md.finish();
    {
        maya::testing::advance_anim_clock_ms(kFrameMs);
        Canvas c = paint();
        int cells = content_cells(c);
        stats.push_back({drain_section, cells - prev_cells, content_rows(c)});
        prev_cells = cells;
    }

    // ── Report ──
    std::printf("frames=%zu drain_frames=%d shrink_events=%d\n\n",
                stats.size(), drain_frames, shrink_events);
    std::printf("%-10s %7s %7s %7s %7s %7s %8s\n",
                "section", "frames", "mean", "p95", "max", "zeros", "zrun");
    for (int sec = 0; sec <= static_cast<int>(doc.size()); ++sec) {
        std::vector<int> d;
        int zeros = 0, zrun = 0, cur_zrun = 0;
        for (auto& f : stats) {
            if (f.section != sec) continue;
            d.push_back(f.delta_cells);
            if (f.delta_cells <= 0) { ++zeros; cur_zrun++; zrun = std::max(zrun, cur_zrun); }
            else cur_zrun = 0;
        }
        if (d.empty()) continue;
        std::vector<int> sorted = d;
        std::sort(sorted.begin(), sorted.end());
        long sum = 0; for (int v : d) sum += v;
        int mean = static_cast<int>(sum / static_cast<long>(d.size()));
        int p95  = sorted[static_cast<std::size_t>(sorted.size() * 95 / 100)];
        int mx   = sorted.back();
        const char* name = sec < static_cast<int>(doc.size())
                         ? doc[static_cast<std::size_t>(sec)].name : "drain";
        std::printf("%-10s %7zu %7d %7d %7d %7d %8d\n",
                    name, d.size(), mean, p95, mx, zeros, zrun);
    }

    // Per-frame trace of the worst bursts for eyeballing.
    std::printf("\nworst 12 frames:\n");
    std::vector<std::size_t> idx(stats.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return stats[a].delta_cells > stats[b].delta_cells; });
    for (std::size_t k = 0; k < 12 && k < idx.size(); ++k) {
        auto& f = stats[idx[k]];
        const char* name = f.section < static_cast<int>(doc.size())
                         ? doc[static_cast<std::size_t>(f.section)].name : "drain";
        std::printf("  frame %5zu  section=%-8s  +%d cells  rows=%d\n",
                    idx[k], name, f.delta_cells, f.rows);
    }

    // ── Gates ──
    //
    // (1) No height shrink at any frame: the monotonicity invariant that
    //     keeps committed scrollback rows immutable.
    // (2) No single frame reveals more than kBurstCap content cells. The
    //     pre-gate regression (block commits outrunning the reveal cursor)
    //     popped whole blocks: code +196, lists +191, tables +103 per
    //     frame vs prose's ~2-6. Healthy post-gate maxima sit ≤ ~22
    //     (settle-ramp acceleration on the final paragraph). 60 gives
    //     noise headroom while still catching the +100-class pop.
    int fails = 0;
    if (shrink_events != 0) {
        std::printf("FAIL: %d height-shrink event(s) — reveal must be "
                    "monotonic\n", shrink_events);
        ++fails;
    }
    constexpr int kBurstCap = 60;
    for (std::size_t i = 0; i < stats.size(); ++i) {
        if (stats[i].delta_cells > kBurstCap) {
            const char* name = stats[i].section < static_cast<int>(doc.size())
                ? doc[static_cast<std::size_t>(stats[i].section)].name : "drain";
            std::printf("FAIL: frame %zu (section %s) revealed %d content "
                        "cells in one frame (cap %d) — a block popped whole "
                        "instead of gliding\n",
                        i, name, stats[i].delta_cells, kBurstCap);
            ++fails;
        }
    }
    std::printf("\n%s\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
