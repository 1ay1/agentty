// SPDX-License-Identifier: Apache-2.0
//
// compaction_threshold_test.cpp — the smart auto-compaction trigger math.
//
// StreamState::compaction_threshold() turns the model's context window +
// the user's `autocompact_pct` knob into the token count at which background
// compaction fires. The invariants this locks:
//   • percent-of-window (not a fixed absolute margin) so behaviour is
//     consistent across a 200k and a 1M window;
//   • an output-headroom FLOOR (kMinOutputHeadroom) that the threshold can
//     never exceed, so the next turn always has room to answer;
//   • the pct is clamped to [50, 95] and 0 means "use the default";
//   • a 1M-window model rides far deeper than a 200k one before compacting
//     (the exact "goes to 400k without a problem" case).

#include "agentty/domain/session.hpp"

#include <cassert>
#include <cstdio>

using agentty::StreamState;

namespace {

int threshold_for(int context_max, int pct) {
    StreamState s;
    s.context_max = context_max;
    s.autocompact_pct = pct;
    return s.compaction_threshold();
}

} // namespace

int main() {
    constexpr int kDef = StreamState::kDefaultAutocompactPct;   // 90
    constexpr int kFloor = StreamState::kMinOutputHeadroom;     // 20000

    // ── default (pct == 0) uses kDefaultAutocompactPct ────────────────────
    {
        // 200k window at 90% = 180k, and 200k - 20k = 180k → floor not binding.
        assert(threshold_for(200000, 0) == 200000 * kDef / 100);
        assert(threshold_for(200000, 0) == 180000);
        std::puts("threshold: default pct on 200k window = 180k");
    }

    // ── 1M window rides MUCH deeper (the 400k-is-fine case) ───────────────
    {
        const int t = threshold_for(1000000, 0);   // 90% = 900k
        assert(t == 900000);
        // Crucially, a conversation at 400k is nowhere near the trigger — no
        // premature compaction on a big-window model.
        assert(400000 < t);
        std::puts("threshold: 1M window fires at 900k — 400k never compacts");
    }

    // ── the pct knob: higher = deeper ─────────────────────────────────────
    {
        assert(threshold_for(1000000, 75) == 750000);
        assert(threshold_for(1000000, 90) == 900000);
        // 95% of 1M = 950k, and 1M - 20k floor = 980k → pct wins.
        assert(threshold_for(1000000, 95) == 950000);
        assert(threshold_for(1000000, 75) < threshold_for(1000000, 95));
        std::puts("threshold: higher pct rides deeper (75<90<95)");
    }

    // ── output-headroom FLOOR caps the threshold on small windows ─────────
    {
        // 40k window at 95% = 38k, but 40k - 20k floor = 20k → floor wins,
        // so we never leave less than kMinOutputHeadroom for the reply.
        const int t = threshold_for(40000, 95);
        assert(t == 40000 - kFloor);
        assert(t == 20000);
        std::puts("threshold: output-headroom floor caps a high pct on a small window");
    }

    // ── pct is clamped to [50, 95] ────────────────────────────────────────
    {
        // 40 clamps up to 50; 99 clamps down to 95.
        assert(threshold_for(1000000, 40) == threshold_for(1000000, 50));
        assert(threshold_for(1000000, 99) == threshold_for(1000000, 95));
        std::puts("threshold: pct clamped to [50, 95]");
    }

    // ── unknown window never fires ────────────────────────────────────────
    {
        assert(threshold_for(0, 90) == 0);
        assert(threshold_for(-5, 90) == 0);
        std::puts("threshold: unknown/zero window never triggers");
    }

    std::puts("ALL COMPACTION-THRESHOLD TESTS PASSED");
    return 0;
}
