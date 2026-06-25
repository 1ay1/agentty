// reveal_pacing_test.cpp — guards the streaming-reveal "constant glide"
// invariant against the long-turn BURST regression.
//
// Symptom the user reported: on a long, fast assistant reply the typewriter
// reveal "starts bursting" — the text races by instead of gliding. Root
// cause (found by reading maya::anim::RateCursor): the cursor auto-tuned its
// cruise speed toward backlog / drain_secs_. Whenever the model streams
// faster than the configured cruise (always, for a fast cloud model on a
// long turn), the backlog grows and the auto-tune chases it up to thousands
// of codepoints/sec — an instant paste, not a glide.
//
// The fix clamps the auto-tuned cruise to floor_rate_ * cruise_ceiling_mult_
// (the constant-glide model every polished chat UI uses: ChatGPT / Claude /
// Vercel smoothStream hold a steady visual rate and let the buffer absorb the
// wire's burstiness). This test pins that:
//
//   1. constant_cruise_under_fast_stream — feed the cursor a target that
//      grows MUCH faster than the cruise speed (a fast model dumping a long
//      reply). Assert the cursor's per-second advance NEVER exceeds the
//      readable ceiling for any sustained window — i.e. it does not burst.
//
//   2. cruise_tracks_a_slow_stream — a slow byte-by-byte feed reveals at the
//      floor speed (no starvation, no stall).
//
//   3. finalize_ramp_still_flushes — the end-of-stream ramp bypasses the
//      cruise cap so the tail always lands by its deadline (the cap must not
//      strand buffered text at settle).
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

// ── 1. No burst under a fast stream ─────────────────────────────────────────
//
// floor = 90 cps cruise, default ceiling mult (2.5×) → 225 cps cruise cap,
// instantaneous cap = cruise_cap * max_burst_mult (1.8×) ≈ 405 cps. Feed a
// target that climbs at 2000 cps (a fast model). The cursor must stay a glide:
// no one-second window may advance more than the instantaneous ceiling, and
// the SUSTAINED rate must settle at the cruise cap, not chase the 2000 cps
// wire.
void constant_cruise_under_fast_stream() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;

    const double floor_cps    = 90.0;
    const double ceiling_mult = 2.5;          // RateCursor default
    const double burst_mult   = 1.8;          // RateCursor default
    const double cruise_cap   = floor_cps * ceiling_mult;          // 225
    const double inst_cap     = cruise_cap * burst_mult * 1.05;    // 5% slack

    double target = 0.0;
    double prev_pos = 0.0;

    double worst_window_cps = 0.0;    // worst per-second advance, sampled each frame
    double window_advance = 0.0;
    double window_time = 0.0;

    // Run 20 s of streaming at 2000 cps.
    for (int frame = 0; frame < 20 * 60; ++frame) {
        target += 2000.0 * dt;        // fast wire
        const double pos = rc.tick(target, dt);
        const double adv = pos - prev_pos;
        prev_pos = pos;

        window_advance += adv;
        window_time += dt;
        if (window_time >= 1.0) {
            const double cps = window_advance / window_time;
            if (cps > worst_window_cps) worst_window_cps = cps;
            window_advance = 0.0;
            window_time = 0.0;
        }
    }

    std::println("  fast stream: worst 1s window = {:.0f} cps "
                 "(cruise cap {:.0f}, inst cap {:.0f})",
                 worst_window_cps, cruise_cap, inst_cap);

    // The cursor must NOT burst: no 1-second window may exceed the
    // instantaneous ceiling. (The OLD unbounded auto-tune hit ~2700 cps here.)
    check(worst_window_cps <= inst_cap,
          "reveal cursor BURSTS under a fast stream: a 1s window advanced " +
          std::to_string(worst_window_cps) + " cps, above the readable "
          "ceiling " + std::to_string(inst_cap) + " — the auto-tuned cruise "
          "is chasing the wire backlog instead of holding a constant glide");

    // And it must actually be moving at roughly the cruise cap (it's behind a
    // huge backlog, so it should pin at the ceiling, not crawl at the floor).
    check(worst_window_cps >= cruise_cap * 0.8,
          "reveal cursor is too slow under backlog (worst window " +
          std::to_string(worst_window_cps) + " cps) — it should cruise near "
          "the ceiling " + std::to_string(cruise_cap) + " when behind");
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

// ── 3. Finalize ramp bypasses the cap ───────────────────────────────────────
void finalize_ramp_still_flushes() {
    RateCursor rc{/*floor*/ 90.0, /*drain*/ 0.3};
    const double dt = 1.0 / 60.0;

    // Build a large backlog the cruise cap can't clear quickly: target jumps
    // to 30000 cp instantly (model dumped the whole reply).
    const double target = 30000.0;
    // Pre-roll a few seconds of normal cruise — the cursor falls way behind.
    for (int i = 0; i < 3 * 60; ++i) rc.tick(target, dt);
    const double behind = target - rc.pos();
    check(behind > 1000.0,
          "test setup: cursor should be far behind a 30k dump after 3s of "
          "capped cruise (was " + std::to_string(behind) + " cp behind)");

    // Arm a 0.5 s finalize ramp: it MUST reach the edge within ~0.5 s
    // regardless of the cruise cap.
    double remaining = 0.5;
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
          "finalize ramp did NOT reach the live edge — the cruise cap stranded "
          "buffered text at settle (the tail would never finish revealing)");
    check(secs <= 0.6,
          "finalize ramp took " + std::to_string(secs) + "s (> deadline) — the "
          "cruise cap is throttling the ramp; the ramp must bypass it");
}

} // namespace

int main() {
    std::println("=== reveal_pacing_test ===");
    constant_cruise_under_fast_stream();
    cruise_tracks_a_slow_stream();
    finalize_ramp_still_flushes();
    std::println("\n  passed: {}   failed: {}", (3 - g_failed), g_failed);
    return g_failed == 0 ? 0 : 1;
}
