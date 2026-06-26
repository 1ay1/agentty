// reveal_pacing_test.cpp — guards the streaming-reveal glide against BOTH
// failure modes the pacing has pendulum'd between:
//
//   • BURST / TELEPORT — the cursor reveals a fat chunk in a single frame
//     (instant paste), so the typewriter "stops" and text just appears. This
//     was re-introduced by the lag-cap SNAP (it teleported pos_ to the edge
//     on any chunk bigger than the lag window).
//   • CRAWL then DUMP — the cursor's speed is capped well below the model's,
//     so on a fast/long reply it falls further and further behind, then dumps
//     the buffered remainder at settle. This was the cruise-ceiling clamp.
//
// The cursor resolves the tension with a rate-smoothed bounded-lag glide:
// rate = backlog / drain_secs_ (TRACKS the model's speed with a bounded TIME
// lag) but the rate itself is low-passed (a chunk accelerates in over a few
// frames instead of teleporting). This file pins that:
//
//   1. tracks_fast_stream_no_crawl_no_teleport — a smooth, fast wire. The
//      cursor must KEEP UP (bounded end-lag, no crawl) yet never advance more
//      than a small multiple of the per-frame arrival in any single frame (no
//      teleport / instant paste).
//
//   2. cruise_tracks_a_slow_stream — a slow byte-by-byte feed reveals at the
//      wire speed (no starvation, no run-ahead).
//
//   3. finalize_ramp_still_flushes — the end-of-stream ramp drives the cursor
//      to the edge by its wall-clock deadline, bypassing the glide smoothing.
//
// Pure RateCursor (header-only); no markdown / no I/O. Drives tick() on a
// fixed 60 fps dt so the math is deterministic and machine-independent.

#include "maya/core/animation.hpp"

#include <cstdio>
#include <print>
#include <string>

using maya::anim::RateCursor;

namespace {

int g_failed = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::println("  FAIL: {}", msg);
        ++g_failed;
    }
}

// ── 1. Fast stream: track it, don't crawl, don't teleport ───────────────────
//
// floor = 90 cps, lead = 0.3 s (the host config). Feed a smooth wire that
// climbs at 2000 cps — faster than any readable typewriter, the regime where
// the old cruise clamp crawled and the old lag cap teleported. The rate-
// smoothed glide must do NEITHER: it tracks the wire (bounded end-lag, ~the
// lead window, NOT tens of thousands of cp behind) while never pasting a fat
// chunk in a single frame (each frame advances only a small multiple of what
// arrived that frame).
void tracks_fast_stream_no_crawl_no_teleport() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;
    const double wire = 2000.0;                 // cp/s, smooth
    const double per_frame_arrival = wire * dt; // ~33 cp/frame

    double target = 0.0, prev = 0.0;
    double max_frame_adv = 0.0;
    for (int frame = 0; frame < 20 * 60; ++frame) {
        target += wire * dt;
        const double pos = rc.tick(target, dt);
        const double adv = pos - prev;
        prev = pos;
        if (adv > max_frame_adv) max_frame_adv = adv;
    }
    const double final_lag = target - rc.pos();

    std::println("  fast stream: end-lag {:.0f} cp (lead window {:.0f} cp), "
                 "max single-frame advance {:.0f} cp (arrival {:.0f} cp/frame)",
                 final_lag, wire * 0.3, max_frame_adv, per_frame_arrival);

    // KEEPS UP: the cursor rides ~drain_secs behind the live edge, NOT a
    // growing backlog. (The old cruise clamp left it ~30 000 cp behind after
    // 20 s at this wire.) Allow a few× the lead window for transient slack.
    check(final_lag <= wire * 0.3 * 4.0,
          "reveal cursor CRAWLS behind a fast wire: end-lag " +
          std::to_string(final_lag) + " cp, far past the ~" +
          std::to_string(wire * 0.3) + " cp lead window — the speed is capped "
          "below the model's, so it falls behind and dumps at settle");

    // NO TELEPORT: a smooth wire delivers ~33 cp/frame; the cursor may lean
    // forward to clear a transient lag but must never paste a big chunk in
    // one frame (the lag-cap SNAP advanced hundreds of cp in a single frame).
    check(max_frame_adv <= per_frame_arrival * 8.0,
          "reveal cursor TELEPORTED: a single frame advanced " +
          std::to_string(max_frame_adv) + " cp, far above the ~" +
          std::to_string(per_frame_arrival) + " cp that arrived that frame — "
          "the chunk pasted instead of sliding in");
}

// ── 2. Slow stream glides at the floor ──────────────────────────────────────
void cruise_tracks_a_slow_stream() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;

    // Wire delivers 40 cps — slower than the 90 cps floor. The cursor should
    // track it (can't reveal bytes that haven't arrived) and never run ahead.
    double target = 0.0;
    double prev = 0.0;
    double max_adv_cps = 0.0;
    for (int frame = 0; frame < 10 * 60; ++frame) {
        target += 40.0 * dt;
        const double pos = rc.tick(target, dt);
        check(pos <= target + 1e-6, "cursor ran past the live edge on a slow feed");
        const double cps = (pos - prev) / dt;
        if (cps > max_adv_cps) max_adv_cps = cps;
        prev = pos;
    }
    // Cursor stays at/under the arrived edge; since wire < floor it reveals at
    // ~wire speed, never bursting.
    std::println("  slow stream (40 cps wire): peak instant {:.0f} cps", max_adv_cps);
    check(max_adv_cps <= 90.0 * 1.8 * 1.05,
          "cursor sped up on a slow feed (peak " + std::to_string(max_adv_cps) +
          " cps) — should track the wire at ~40 cps");
}

// ── 3. Finalize ramp bypasses the glide smoothing ───────────────────────────
void finalize_ramp_still_flushes() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;

    // A whole-reply dump lands at once: a 30 000 cp backlog on one frame. The
    // rate-smoothed glide would slide that in over a few hundred ms; at
    // end-of-stream we instead want it GUARANTEED on screen by a wall-clock
    // deadline. Register the backlog (one tick, no deadline), then arm a
    // 0.2 s ramp and assert the cursor reaches the edge within it — the ramp
    // must bypass the smoothing.
    const double target = 30000.0;
    rc.tick(target, dt);                  // register the backlog, glide seeds low
    const double behind = target - rc.pos();
    check(behind > 1000.0,
          "test setup: a 30k dump should leave a big backlog on the first "
          "frame (was " + std::to_string(behind) + " cp behind)");

    double remaining = 0.2;
    int frames = 0;
    while (rc.pos() < target - 1.0 && frames < 60 /* 1s safety */) {
        rc.set_deadline(remaining);
        rc.tick(target, dt);
        remaining -= dt;
        ++frames;
    }
    const double secs = frames * dt;
    std::println("  finalize ramp: cleared {:.0f} cp backlog in {:.2f}s",
                 behind, secs);
    check(rc.pos() >= target - 1.0,
          "finalize ramp did NOT reach the live edge — buffered text stranded "
          "at settle (the tail would never finish revealing)");
    check(secs <= 0.25,
          "finalize ramp took " + std::to_string(secs) + "s, past its 0.2s "
          "deadline — the glide smoothing is throttling the ramp; the ramp "
          "must bypass it");
}

// ── 4. A fat chunk slides in — it does NOT teleport ─────────────────────────
//
// THE regression test for the user-reported "streaming stops / text just
// appears on a long turn": a proxy- or SSE-batched delivery drops a fat chunk
// (here 500 cp) into a SINGLE frame. The old lag cap SNAPPED pos_ to
// (edge − lag_window), pasting ~275 cp in that one frame — the typewriter
// visibly stopped and the block appeared at once. The rate-smoothed glide
// must instead reveal the chunk across many frames.
void fat_chunk_slides_not_teleports() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;

    // Warm the glide at a moderate steady rate so it isn't cold-starting.
    double target = 0.0;
    for (int f = 0; f < 60; ++f) { target += 200.0 * dt; rc.tick(target, dt); }

    // A fat 500 cp chunk lands in ONE frame (the batched-delivery spike).
    target += 500.0;
    const double before = rc.pos();
    rc.tick(target, dt);
    const double first_frame_adv = rc.pos() - before;

    // Drain it out (no further arrivals) and count the frames.
    int frames = 1;
    while (rc.pos() < target - 1.0 && frames < 600) {
        rc.tick(target, dt);
        ++frames;
    }
    std::println("  fat chunk: first-frame advance {:.0f} cp, fully revealed "
                 "over {} frames ({:.2f}s)", first_frame_adv, frames,
                 frames * dt);

    // NO TELEPORT: the frame the chunk arrives must reveal only a small slice,
    // not paste the bulk. (The lag-cap SNAP advanced ~275 cp here.)
    check(first_frame_adv < 120.0,
          "fat chunk TELEPORTED: the arrival frame advanced " +
          std::to_string(first_frame_adv) + " cp — the chunk pasted instead of "
          "sliding in (the typewriter 'stops')");

    // And it slides in over a sane, animated window: several frames of motion,
    // but not a multi-second crawl.
    check(frames >= 5 && frames * dt <= 1.5,
          "fat chunk drain window unreasonable (" + std::to_string(frames) +
          " frames) — should be a brief animated slide, not instant or a crawl");
}

} // namespace

int main() {
    std::println("=== reveal_pacing_test ===");
    tracks_fast_stream_no_crawl_no_teleport();
    cruise_tracks_a_slow_stream();
    finalize_ramp_still_flushes();
    fat_chunk_slides_not_teleports();
    std::println("\n  passed: {}   failed: {}", (4 - g_failed), g_failed);
    return g_failed == 0 ? 0 : 1;
}
