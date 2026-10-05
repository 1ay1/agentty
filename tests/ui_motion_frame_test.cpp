// ui_motion_frame_test — a live Appearance change reaches EVERY streaming
// widget, not just the first one rendered.
//
// ── The bug ─────────────────────────────────────────────────────────────
//
// turn.cpp re-applies the reveal-fx policy to a streaming widget when the
// widget is fresh OR when the motion pref just changed. The second term is
// what makes "turn motion off" take effect mid-stream rather than on the
// next message.
//
// It was a compare-and-reset latch local to cached_markdown_for():
//
//     static Motion last_motion = current().motion;
//     const bool motion_changed = current().motion != last_motion;
//     last_motion = current().motion;        // <-- consumed HERE
//
// But cached_markdown_for() runs SEVERAL times per frame — once per streaming
// widget. On a turn that is both thinking and answering that is the reasoning
// lane (the "#r" cache slot) and the answer lane, plus one call per sub-turn
// in a multi-message turn. reasoning_slot() renders above the answer body, so
// the reasoning lane was reached first, consumed the change, and every later
// widget in the same frame read `motion_changed == false`.
//
// Net effect: toggling motion mid-stream applied to the reasoning block and
// left the answer body animating. turn.cpp already carries a paragraph
// warning about this exact failure ("not once per process, so every message's
// widget gets the policy rather than only the first") — the latch
// reintroduced it through the back door.
//
// ── Why the fix is per-widget and not a frame flag ──────────────────────
//
// The first attempt was a process-global "motion changed this frame" flag set
// by publish(). It fixes the sharing, and is worse: the flag is STICKY for
// anything that does not publish every frame, so it leaked out of this very
// test file and broke midrun_wire_test (which seeds reveal_fx OFF to measure
// a settled height, and had it switched back on by a spurious re-apply).
//
// The honest condition was never about frames. It is "does this widget's
// configured policy differ from the one in force" — per-widget state, which
// is what MessageMdCache::applied_motion holds. No ordering, no frame
// semantics, nothing to leak.
//
// This test pins the DECISION RULE against that model.

#include "agtest.hpp"

#include "agentty/domain/ui_live.hpp"

using agentty::ui_prefs::Motion;

namespace {

// The rule as turn.cpp applies it: re-apply when the widget is fresh, or when
// what it holds KNOWABLY differs from what is in force; then stamp what was
// applied. applied_motion < 0 means "never set by us" — not stale — so it
// does not by itself force a re-apply (a slot seeded from outside keeps the
// policy it was given).
struct Widget {
    int applied_motion = -1;            // -1 = never stamped
    bool fresh = true;                  // true only on the frame we create it
    int applies = 0;                    // how many times the policy was set

    void frame(Motion in_force) {
        const int now = static_cast<int>(in_force);
        const bool changed = applied_motion >= 0 && applied_motion != now;
        if (fresh || changed) {
            ++applies;
            applied_motion = now;
        }
        fresh = false;
    }
};

}  // namespace

TEST_CASE("ui motion: every widget in the frame sees the change") {
    // Two lanes of ONE turn: reasoning renders first, then the answer.
    Widget reasoning, answer;

    // Steady state at Full — each configures once on its first frame.
    reasoning.frame(Motion::Full);
    answer.frame(Motion::Full);
    CHECK(reasoning.applies == 1);
    CHECK(answer.applies == 1);

    // The user turns motion OFF mid-stream. BOTH lanes must re-apply in the
    // same frame. Under the old shared latch the reasoning lane consumed the
    // change and the answer lane stayed at 1 — the bug.
    reasoning.frame(Motion::Off);
    answer.frame(Motion::Off);
    CHECK(reasoning.applies == 2);
    CHECK(answer.applies == 2);
}

TEST_CASE("ui motion: a steady pref does not re-apply every frame") {
    // The reason the condition exists at all: re-applying rebuilds the
    // widget, so an always-true test would rebuild every streaming widget on
    // every frame of every turn.
    Widget w;
    for (int i = 0; i < 20; ++i) w.frame(Motion::Full);
    CHECK(w.applies == 1);
}

TEST_CASE("ui motion: every level transition re-applies") {
    // The reveal policy differs at all three levels (Reduced keeps the
    // progressive clip and drops the decorative churn), so no pair may be
    // treated as equivalent.
    const Motion levels[] = {Motion::Full, Motion::Reduced, Motion::Off};
    for (const Motion from : levels) {
        for (const Motion to : levels) {
            if (from == to) continue;
            Widget w;
            w.frame(from);
            const int before = w.applies;
            w.frame(to);
            CHECK(w.applies == before + 1);
        }
    }
}

TEST_CASE("ui motion: a widget seeded from outside keeps its policy") {
    // A slot built directly (midrun_wire_test seeds reveal_fx OFF to measure
    // a settled height) is not ours to reconfigure. An unstamped level means
    // "never set by us", so it must not read as stale and get overwritten --
    // which is exactly what an earlier version of this fix did.
    Widget seeded;
    seeded.fresh = false;              // not created by the view this frame
    seeded.frame(Motion::Full);
    CHECK(seeded.applies == 0);

    // ...but once the pref actually MOVES there is still nothing to compare
    // against, so it stays untouched. The widget is adopted only when the
    // view creates it.
    seeded.frame(Motion::Off);
    CHECK(seeded.applies == 0);
}

TEST_CASE("ui motion: a late-created widget adopts the level in force") {
    // A sub-turn whose widget is created AFTER the pref moved must configure
    // itself from the current level, not inherit a stale one. Freshness is
    // the first term of the condition precisely so this holds.
    Widget late;
    late.frame(Motion::Off);
    CHECK(late.applies == 1);
    CHECK(late.applied_motion == static_cast<int>(Motion::Off));
    // And it does not then re-apply on the next frame.
    late.frame(Motion::Off);
    CHECK(late.applies == 1);
}
