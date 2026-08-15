// reveal_resume_test.cpp — property test for the finalize-ramp DISARM and
// clock-carry seams found in the reveal audit (docs/STREAMING_REVEAL.md,
// "Audit fixes" #1/#2/#3). These are the timing edges steady streaming never
// crosses, so neither the det harness (single finalize at end) nor the
// pacing test (pure RateCursor) exercises them:
//
//   1. resume_after_finalize_disarms (#2) — request_finalize() fires
//      MID-MESSAGE (the host does this at a text→tool gap / closed text
//      block), the cursor lands, the ramp's settle window is still running…
//      and then MORE TEXT arrives. The ramp must DISARM (is_finalizing()
//      false) and the widget must return to normal jitter-buffered pacing —
//      not stay wedged in ramp mode cruising ≥2× floor for the rest of the
//      turn (the pre-fix behavior: every frame the #4 re-eval rolled the
//      deadline forward, collapsing the drain_secs buffer).
//
//   2. hard_snap_deadline_honoured (#1) — snap_reveal_to_edge(glide_ms)
//      promises a HARD wall-clock deadline against a big backlog. The
//      adaptive #4 re-eval must NOT stretch it (pre-fix: a 300-cp backlog
//      at floor 45 stretched a 150 ms tool-seam snap to 2.5 s, holding
//      deferred tool cards hidden the whole time).
//
//   3. no_paste_after_idle_gap (#3) — the model goes silent at the edge,
//      frames STOP (the host's caret window expires after 4 s), several
//      seconds pass with NO build() calls, then a chunk arrives. The stale
//      µs clock must not be integrated as "owed" typewriter time: the
//      first frames after resume must reveal at glide pace, not drain the
//      idle gap at kMaxCatchupS/frame (~15× real time = a paste).
//
// Drives the REAL StreamingMarkdown widget on the frozen test clock at a
// deterministic 16 ms cadence. Progress is measured via debug_reveal_cp()
// (the cursor), which is what the byte clip — and thus everything visible —
// derives from.

#include <maya/widget/markdown.hpp>
#include <maya/core/anim_clock.hpp>

#include <print>
#include <string>

namespace {

int g_failed = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::println("  FAIL: {}", msg);
        ++g_failed;
    }
}

constexpr std::int64_t kFrameMs = 16;

// One 16 ms frame: build() (which advances the cursor) then tick the clock.
void frame(maya::StreamingMarkdown& md) {
    (void)md.build();
    maya::testing::advance_anim_clock_ms(kFrameMs);
}

// ── 1. (#2) resume after a mid-message finalize must disarm the ramp ───────
void resume_after_finalize_disarms() {
    std::println("--- resume_after_finalize_disarms ---");
    maya::testing::freeze_anim_clock(0);
    maya::StreamingMarkdown md;
    md.set_live(true);
    md.set_reveal_fx(true);
    md.set_reveal_pacing(/*floor_cps*/ 45.0, /*drain_secs*/ 0.40);

    // Stream a paragraph, let the cursor start gliding.
    md.append("The quick brown fox jumps over the lazy dog. ");
    for (int i = 0; i < 10; ++i) frame(md);

    // Host decides the text block closed → mid-message finalize.
    md.request_finalize(200);
    check(md.is_finalizing(), "ramp armed after request_finalize");

    // Run until the ramp LANDS the cursor at the edge (the adaptive arm
    // may stretch the 200 ms request for this backlog; cap generously).
    // Verify the landing POSITIVELY — the disarm keys off the cursor
    // having reached the armed-time edge, so the premise must hold.
    bool landed = false;
    for (int t = 0; t <= 1500; t += kFrameMs) {
        frame(md);
        if (md.debug_reveal_byte_clip() >= md.debug_source_size()) {
            landed = true;
            break;
        }
    }
    check(landed, "ramp landed the cursor at the edge before resume");
    check(md.is_live(), "still live during settle window");

    // The stream RESUMES: an interleaved text block / next delta.
    md.append("Actually, there is more prose arriving now — a lot more "
              "text than the ramp was armed against, streamed on.");
    frame(md);

    // #2: growth after the cursor reached the armed-time edge must disarm.
    check(!md.is_finalizing(),
          "ramp DISARMED when the stream resumed at the edge "
          "(pre-fix: wedged in ramp mode for the rest of the turn)");

    // And the jitter buffer must be BACK: drip further deltas at a steady
    // wire pace and verify the cursor rides BEHIND the edge again (the
    // ≈drain_secs lag), instead of being pinned to the edge by a wedged
    // ramp (whose rolling deadline forces cursor→edge every frame, lag 0).
    for (int i = 0; i < 60; ++i) {          // ~1 s of steady drip
        if (i % 2 == 0) md.append("more steady prose flows "); // ~12 cps/frame pair
        frame(md);
    }
    const double total_cp_now = static_cast<double>(md.debug_source_size());
    const double lag = total_cp_now - md.debug_reveal_cp();
    check(lag > 4.0,
          std::format("jitter-buffer lag restored after disarm "
                      "(cursor rides {:.1f} cp behind the edge; a wedged "
                      "ramp pins it at ~0)", lag));
    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

// ── 2. (#1) hard snap deadline must be honoured against a big backlog ──────
void hard_snap_deadline_honoured() {
    std::println("--- hard_snap_deadline_honoured ---");
    maya::testing::freeze_anim_clock(0);
    maya::StreamingMarkdown md;
    md.set_live(true);
    md.set_reveal_fx(true);
    md.set_reveal_pacing(45.0, 0.40);

    // Build a BIG backlog: ~600 cp arrives at once, cursor near 0.
    std::string big;
    for (int i = 0; i < 12; ++i)
        big += "This sentence pads the backlog with roughly fifty chars. ";
    md.append(big);
    frame(md);  // cursor starts near 0, backlog ~600 cp

    // Tool boundary: the host promises 150 ms is scrollback-safe.
    md.snap_reveal_to_edge(150);
    check(md.is_finalizing(), "hard glide armed");

    // The cursor must reach the edge within 150 ms + a small frame slop.
    // Pre-fix, the #4 re-eval stretched this to backlog/(2·45) ≈ 2500 ms.
    const double total = static_cast<double>(md.debug_source_size());
    bool landed = false;
    std::int64_t t = 0;
    for (; t <= 150 + 3 * kFrameMs; t += kFrameMs) {
        frame(md);
        if (md.debug_reveal_byte_clip() >= md.debug_source_size()) {
            landed = true;
            break;
        }
    }
    check(landed,
          std::format("cursor reached the edge by the hard 150 ms deadline "
                      "(landed={} at t={} ms, clip={}/{})",
                      landed, t, md.debug_reveal_byte_clip(),
                      static_cast<std::size_t>(total)));
    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

// ── 3. (#3) a chunk after a frameless idle gap must glide, not paste ───────
void no_paste_after_idle_gap() {
    std::println("--- no_paste_after_idle_gap ---");
    maya::testing::freeze_anim_clock(0);
    maya::StreamingMarkdown md;
    md.set_live(true);
    md.set_reveal_fx(true);
    md.set_reveal_pacing(45.0, 0.40);

    // Stream some text and let the cursor fully catch up to the edge.
    md.append("A first paragraph that the cursor fully reveals. ");
    for (int i = 0; i < 120; ++i) frame(md);  // ~1.9 s, plenty at 45 cps

    // Model goes SILENT. The host stops calling build() after the caret
    // window — simulate the frameless gap by advancing the clock 8 s with
    // NO build() calls (this is what leaves the µs clock stale).
    maya::testing::advance_anim_clock_ms(8000);

    // A new chunk lands (~190 cp).
    std::string chunk;
    for (int i = 0; i < 4; ++i)
        chunk += "Fresh prose arriving after the long model silence. ";
    md.append(chunk);

    // First frames after resume: the per-frame cursor advance must be
    // glide-sized. Pre-fix, the stale 8 s clock drained at 250 ms of
    // cursor-motion per frame: 45 cps × 0.25 s ≈ 11+ cp EVERY frame plus
    // the burst term — the whole chunk pasted in a handful of frames.
    // Post-fix the first frame integrates dt≈0 and subsequent frames a
    // real 16 ms: ≤ a few cp each.
    double max_step = 0.0;
    double prev = md.debug_reveal_cp();
    for (int i = 0; i < 12; ++i) {
        frame(md);
        const double cur = md.debug_reveal_cp();
        if (cur - prev > max_step) max_step = cur - prev;
        prev = cur;
    }
    // Glide bound: floor 45 cps, drain 0.40 s ⇒ steady rate for a 190-cp
    // backlog ≈ backlog/drain = 475 cps → 7.6 cp/frame, low-passed lower on
    // the first frames. The pre-fix pathological step was ≥11 cp (stale-
    // clock 250 ms slices) stacked on that. Cap at 10: passes the honest
    // glide, fails the stale-clock drain.
    check(max_step > 0.0, "cursor resumed after the idle gap");
    check(max_step <= 10.0,
          std::format("per-frame step stayed glide-sized after an 8 s "
                      "frameless gap (max {:.1f} cp/frame; stale-clock "
                      "drain would exceed it)", max_step));
    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

} // namespace

int main() {
    resume_after_finalize_disarms();
    hard_snap_deadline_honoured();
    no_paste_after_idle_gap();
    if (g_failed) {
        std::println("{} check(s) FAILED", g_failed);
        return 1;
    }
    std::println("All reveal resume/disarm tests passed.");
    return 0;
}
