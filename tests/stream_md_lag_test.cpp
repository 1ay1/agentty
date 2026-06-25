// stream_md_lag_test.cpp — reproduces the "long-turn markdown gets stuck and
// laggy" symptom as a measurable per-frame cost regression.
//
// What the user reports: during a LONG assistant turn (a big markdown reply
// streaming in), the live markdown rendering stutters — the text freezes for a
// beat and then bursts. This test pins the structural cause without a terminal:
// it drives maya::StreamingMarkdown exactly the way agentty's view layer does
// (src/runtime/view/thread/turn/turn.cpp::cached_markdown_for):
//
//   • set_reveal_fx(true)          — animated live-edge reveal ON
//   • set_live(true)               — widget is live (streaming)
//   • set_content(growing_body)    — full content resent every frame
//   • build()                      — assemble the element tree every frame
//
// The work each build() does is the per-frame cost the render loop pays at
// 60 fps. If that cost grows with the TOTAL turn length (instead of staying
// bounded by the small live tail), a long turn eventually can't render within
// a frame budget — exactly the observed stutter.
//
// The test measures the *marginal* per-frame cost two ways:
//
//   1. long_turn_per_frame_cost_stays_bounded — compares the average
//      build() cost in an EARLY growth window (body ≈ 2–6 KB) against a
//      LATE one (≈ 40–60 KB) of the same turn, as bytes stream in.
//
//   2. live_tail_reprocess_stays_bounded — times STEADY (no-delta) repeat
//      frames at 6 KB vs 70 KB. The view layer rebuilds every ~16 ms even
//      when no new bytes arrived; with maya's deferred-commit reveal path
//      the whole turn is the live tail, so each idle animation frame walks
//      O(N) bytes. This is the cleaner, allocation-free signal of the lag.
//
// A renderer whose per-frame cost is O(tail) keeps both ratios near 1; the
// current widget defers ALL block commits while reveal_fx is live (commit.cpp
// "DEFER ALL commits"), so the tail IS the whole body and the per-frame
// reveal decoration scales with N — the stutter the user feels.
//
// NOTE: timing-based, so it asserts a generous bound (not a tight number) to
// stay stable across machines/CI. The point is to catch an ORDER-OF-MAGNITUDE
// per-frame cost growth, which is what the user feels as lag.

#include "maya/widget/markdown.hpp"
#include "maya/element/element.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace maya;
using clock_t_ = std::chrono::steady_clock;

namespace {

int g_failed = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::println("  FAIL: {}", msg);
        ++g_failed;
    }
}

// Committed byte extent of a StreamingMarkdown: the widget has no direct
// committed-byte accessor, but its committed prefix is exactly the union of
// its committed top-level blocks (block_meta(i).source_end is the exclusive
// byte end of block i), so the LAST committed block's source_end is the
// number of bytes that have left the live tail. Zero blocks → nothing
// committed yet.
std::size_t committed_bytes(const StreamingMarkdown& md) {
    const std::size_t n = md.block_count();
    if (n == 0) return 0;
    return md.block_meta(n - 1).source_end;
}

// Build a realistic long markdown reply: many short prose paragraphs
// interleaved with the occasional fenced code block and list. This mirrors
// the shape of a long assistant answer (the regime the user hits). Returns
// the full body; the test streams growing prefixes of it.
std::string make_long_reply(int paragraphs) {
    std::string out;
    out.reserve(static_cast<std::size_t>(paragraphs) * 220);
    for (int i = 0; i < paragraphs; ++i) {
        if (i % 7 == 3) {
            out += "Here is a short example for step ";
            out += std::to_string(i);
            out += ":\n\n```cpp\n";
            out += "int step_" + std::to_string(i) + "(int x) {\n";
            out += "    return x * " + std::to_string(i + 1) + " + 1;\n";
            out += "}\n```\n\n";
        } else if (i % 5 == 2) {
            out += "Key points for section " + std::to_string(i) + ":\n\n";
            out += "- first consideration that matters here\n";
            out += "- second consideration with a bit more detail\n";
            out += "- third consideration to round things out\n\n";
        } else {
            out += "## Section " + std::to_string(i) + "\n\n";
            out += "This paragraph explains part " + std::to_string(i) +
                   " of the answer in enough words to look like a real "
                   "assistant reply, with some **bold** and `inline code` "
                   "so the inline parser has real work to do every frame.\n\n";
        }
    }
    return out;
}

// Drive the widget like the REAL render loop: feed `target` bytes of `body`
// in `step`-byte deltas, calling build() after each. CRUCIALLY we let real
// wall-clock time pass between frames — the reveal cursor (and therefore the
// commit-behind-cursor that bounds the live tail) is paced on steady_clock,
// exactly as it is when the host paints at ~60 fps. A test that pumps frames
// with zero elapsed time freezes the cursor at 0, which never happens in
// production and would measure an unreachable state. `frame_gap` is the
// simulated inter-frame interval.
struct WindowCost {
    double total_us = 0.0;
    int    frames   = 0;
    double per_frame_us() const { return frames ? total_us / frames : 0.0; }
};

WindowCost drive_window(StreamingMarkdown& md, const std::string& body,
                        std::size_t from, std::size_t to, std::size_t step,
                        std::chrono::microseconds frame_gap) {
    WindowCost wc;
    for (std::size_t n = from; n <= to && n <= body.size(); n += step) {
        std::string_view src{body.data(), n};
        auto t0 = clock_t_::now();
        md.set_content(src);
        (void)md.build();
        auto t1 = clock_t_::now();
        wc.total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        ++wc.frames;
        if (frame_gap.count() > 0) std::this_thread::sleep_for(frame_gap);
    }
    return wc;
}

// ── The repro ───────────────────────────────────────────────────────────────
//
// Per-frame cost must not scale with total turn length. We compare the
// average per-frame build cost in an EARLY window (body 2–6 KB) against a
// LATE window (body 40–60 KB) of the SAME turn. With an O(tail) renderer the
// two are within a small constant of each other; with the deferred-commit
// O(N) path the late window is many× more expensive.
void long_turn_per_frame_cost_stays_bounded() {
    const std::string body = make_long_reply(400);
    std::println("  reply body size: {} bytes", body.size());
    check(body.size() >= 60'000,
          "test body must be >=60KB to exercise the long-turn regime");

    StreamingMarkdown md;
    md.set_reveal_fx(true);
    // Fast pacing so the wall-clock reveal cursor keeps up with the streamed
    // bytes within a few frames (production paces slower, but the cursor
    // still catches up via the burst-drain; here we just want the
    // commit-behind-cursor to engage quickly so the test runs fast).
    md.set_reveal_pacing(20'000.0, 0.05);
    md.set_live(true);

    // ~2 ms inter-frame gap — enough wall-clock for the cursor to advance
    // and the live tail to commit behind it, like the real 60 fps loop.
    const auto gap = std::chrono::microseconds(2'000);

    // Warm up from 0 → 2KB so the early window isn't paying first-frame
    // allocation/parse-cache priming.
    (void)drive_window(md, body, 256, 2'000, 256, gap);

    const WindowCost early = drive_window(md, body,  2'000,  6'000, 256, gap);
    const WindowCost late  = drive_window(md, body, 40'000, 60'000, 256, gap);

    std::println("  early window: {} frames, {:.1f} us/frame",
                 early.frames, early.per_frame_us());
    std::println("  late  window: {} frames, {:.1f} us/frame",
                 late.frames,  late.per_frame_us());

    check(early.frames > 0 && late.frames > 0, "both windows must run frames");

    const double ratio = late.per_frame_us() /
                         (early.per_frame_us() > 0.1 ? early.per_frame_us() : 0.1);
    std::println("  per-frame cost ratio (late / early): {:.1f}x", ratio);

    // With commit-behind-cursor the live tail stays bounded (the revealed
    // prefix commits), so per-frame build() cost is O(tail), roughly flat
    // across the turn → ratio near 1–3x (cache warmup + a larger committed
    // prefix the overlay walks). The old deferred-all-commits path kept the
    // whole turn in the tail and re-walked O(N) every frame → ratio blows
    // well past this bound. 6x gates the order-of-magnitude regression.
    check(ratio <= 6.0,
          "per-frame build() cost scales with total turn length (late window "
          "is " + std::to_string(ratio) + "x the early window) — long-turn "
          "markdown rendering is O(N) per frame, which is the streaming lag");
}

// Companion structural assertion: with the reveal cursor advancing on
// wall-clock (as the render loop drives it), the commit-behind-cursor path
// commits every block that lies BEHIND the cursor, so the live tail stays
// bounded to the un-revealed remainder. That keeps the per-frame build()
// (render_tail + the live overlay's tail walk) O(bounded tail), not
// O(total turn). This drives a realistic stream and asserts BOTH: the live
// tail is bounded mid-stream, and steady (no-delta) animation frames on the
// settled body are cheap and flat in body size.
void live_tail_reprocess_stays_bounded() {
    const std::string body = make_long_reply(400);

    // Stream the whole body with wall-clock spacing so the cursor advances
    // and the live tail commits behind it.
    auto stream_and_measure = [&](std::size_t target) {
        StreamingMarkdown md;
        md.set_reveal_fx(true);
        md.set_reveal_pacing(20'000.0, 0.05);
        md.set_live(true);
        const auto gap = std::chrono::microseconds(2'000);
        for (std::size_t n = 256; n <= target && n <= body.size(); n += 512) {
            md.set_content(std::string_view{body.data(), n});
            (void)md.build();
            std::this_thread::sleep_for(gap);
        }
        // Let the cursor reach the edge so the body fully reveals + commits.
        for (int i = 0; i < 40; ++i) {
            (void)md.build();
            std::this_thread::sleep_for(std::chrono::microseconds(2'000));
        }
        // Time steady (no new bytes) frames — the idle-but-live floor.
        const int reps = 200;
        auto t0 = clock_t_::now();
        for (int i = 0; i < reps; ++i) (void)md.build();
        auto t1 = clock_t_::now();
        double steady_us =
            std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
        std::size_t tail = md.source().size() - committed_bytes(md);
        return std::pair<double, std::size_t>{steady_us, tail};
    };

    auto [small_us, small_tail] = stream_and_measure(6'000);
    auto [large_us, large_tail] = stream_and_measure(body.size());
    std::println("  steady frame: {:.2f} us @6KB (tail {}B), "
                 "{:.2f} us @full (tail {}B)",
                 small_us, small_tail, large_us, large_tail);

    // The live tail must be bounded even on the full body — the
    // commit-behind-cursor moved the revealed prefix into committed blocks.
    // (~one screenful of un-revealed bytes remains; certainly not the whole
    // turn.) If this regresses, the deferral-all-commits behaviour is back.
    check(large_tail <= 16'000,
          "live tail is not bounded after a full streamed turn (tail " +
          std::to_string(large_tail) + " bytes) — the revealed prefix isn't "
          "committing behind the cursor, so render_tail re-walks O(turn) "
          "every animation frame (the long-turn lag)");

    const double ratio = large_us / (small_us > 0.1 ? small_us : 0.1);
    std::println("  steady-frame cost ratio (full / 6KB): {:.1f}x", ratio);
    check(ratio <= 6.0,
          "steady live animation frames cost more on a longer turn (ratio " +
          std::to_string(ratio) + "x) — per-frame work still scales with total "
          "turn length, which is the streaming lag");

    // finish() must flush the whole tail through commit_range so the settled
    // turn is fully committed (full chrome) at end-of-stream.
    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(20'000.0, 0.05);
    md.set_live(true);
    md.set_content(body);
    (void)md.build();
    md.finish();
    (void)md.build();
    const std::size_t committed_done = committed_bytes(md);
    std::println("  after finish: committed {} / {} bytes",
                 committed_done, md.source().size());
    check(committed_done >= md.source().size() / 2,
          "finish() did not flush the live tail into committed blocks — the "
          "settled turn is still mostly uncommitted, so the final layout "
          "snap and any post-stream scroll/fold operate on no prefix");
}

} // namespace

int main() {
    std::println("=== stream_md_lag_test ===");
    long_turn_per_frame_cost_stays_bounded();
    live_tail_reprocess_stays_bounded();
    std::println("\n  passed: {}   failed: {}",
                 (2 - g_failed), g_failed);
    return g_failed == 0 ? 0 : 1;
}
