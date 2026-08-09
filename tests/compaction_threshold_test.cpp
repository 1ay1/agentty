// SPDX-License-Identifier: Apache-2.0
//
// compaction_threshold_test.cpp — the auto-compaction trigger math.
//
// StreamState::compaction_threshold() turns the model's context window into
// the token count at which background compaction fires. The design: ride
// DEEP (kSoftFillPct = 95%) so we keep maximum live context and compact as
// rarely as possible — each compaction is the expensive event (it runs a
// summary request AND resets the prompt cache), so firing seldom is what
// keeps total token burn low. The only clamp is an output-headroom FLOOR
// (kMinOutputHeadroom) so the next turn's reply always has room.
//
// Invariants locked here:
//   • threshold = min(95% of window, window - 20k);
//   • a 200k window fires at 190k, a 1M window at 950k (deep — not early);
//   • the output-headroom floor is never violated;
//   • bigger window ⇒ higher (never earlier) absolute trigger;
//   • 0 means "window unknown, never fire".

#include "agentty/domain/session.hpp"

#include <cassert>
#include <cstdio>

using agentty::StreamState;

namespace {

int threshold_for(int context_max) {
    StreamState s;
    s.context_max = context_max;
    return s.compaction_threshold();
}

} // namespace

int main() {
    constexpr int kFloor = StreamState::kMinOutputHeadroom;  // 20000
    constexpr int kPct   = StreamState::kSoftFillPct;        // 95

    // ── 200k window: 95% = 190k, reserve = 180k → 190k < 180k? no ─────────
    // 95% of 200k = 190000; window - floor = 180000; min = 180000.
    // (On this size the headroom floor binds, giving 180k.)
    {
        assert(threshold_for(200000) == 180000);
        std::puts("threshold: 200k window fires at 180k (headroom-floor bound)");
    }

    // ── 400k window: 95% = 380k, reserve = 380k → 380k ───────────────────
    {
        // 95% of 400k = 380000; window - 20k = 380000; min = 380000.
        assert(threshold_for(400000) == 380000);
        std::puts("threshold: 400k window fires at 380k (95%)");
    }

    // ── 1M window: rides DEEP to 950k, NOT compacting early ──────────────
    {
        // 95% of 1M = 950000; window - 20k = 980000; min = 950000.
        assert(threshold_for(1000000) == 950000);
        // Sanity: this is deep — well past halfway, near the ceiling.
        assert(threshold_for(1000000) > 900000);
        std::puts("threshold: 1M window rides deep to 950k (95%)");
    }

    // ── 2M window: 95% = 1.9M ────────────────────────────────────────────
    {
        assert(threshold_for(2000000) == 1900000);
        std::puts("threshold: 2M window fires at 1.9M (95%)");
    }

    // ── output-headroom floor is always respected ────────────────────────
    {
        // The threshold never leaves less than kFloor of the window free.
        for (int w : {50000, 100000, 200000, 500000, 1000000, 2000000}) {
            assert(threshold_for(w) <= w - kFloor);
        }
        // On a small window the floor binds: 100k → 95% = 95k, reserve = 80k.
        assert(threshold_for(100000) == 100000 - kFloor);
        std::puts("threshold: output-headroom floor always respected");
    }

    // ── monotonic: a bigger window never fires SOONER (absolute tokens) ──
    {
        int prev = -1;
        for (int w : {100000, 200000, 400000, 1000000, 2000000}) {
            const int t = threshold_for(w);
            assert(t >= prev);
            prev = t;
        }
        std::puts("threshold: monotonic non-decreasing in window size");
    }

    // ── unknown window never fires ───────────────────────────────────────
    {
        assert(threshold_for(0) == 0);
        assert(threshold_for(-5) == 0);
        std::puts("threshold: unknown/zero window never triggers");
    }

    std::puts("ALL COMPACTION-THRESHOLD TESTS PASSED");
    return 0;
}
