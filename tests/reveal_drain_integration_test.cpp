// reveal_drain_integration_test — proves (and guards) the post-stream reveal
// finalize ordering. The design intent (finalize_turn idle branch + extensive
// comments in stream.cpp) is: at settle, DON'T finish() the reveal — instead
// request_finalize(200) so the typewriter glides to the edge over ~200ms and
// the widget flips live_ off ON ITS OWN, keeping the animation smooth.
//
// THE BUG this guards: finalize_turn's PRE-settle block calls
// settle_message_md() on the back assistant message, and settle_message_md()
// calls finish() — which flips live_ off immediately. The idle branch's
// request_finalize(200) then early-returns (it no-ops when !live_), so the
// glide never runs and the reveal snaps to fully-revealed in one frame: the
// "long-turn md animation jumps / gets stuck instead of typing out."
//
// We reproduce both call orders against the REAL maya::StreamingMarkdown and
// assert the FIXED order leaves the widget live with a finalize ramp armed
// (so it glides), while documenting that the buggy order kills it.

#include <chrono>
#include <cstdio>
#include <string>

#include "maya/widget/markdown.hpp"

namespace {

int g_checks = 0, g_failures = 0;
void check(bool c, const std::string& w) {
    ++g_checks;
    if (c) std::printf("  ok: %s\n", w.c_str());
    else { ++g_failures; std::fprintf(stderr, "  FAIL: %s\n", w.c_str()); }
}

// Build a live, mid-glide reveal widget: reveal_fx on, fed a long body, live,
// one build() so the cursor starts behind the edge — exactly the widget state
// at the instant StreamFinished lands on a long turn.
void make_midglide(maya::StreamingMarkdown& md, const std::string& body) {
    md.set_reveal_fx(true);
    md.set_content(body);
    md.set_live(true);
    (void)md.build();   // cursor starts at 0, far behind a 6000-char edge
}

// Mirror settle_message_md(): set_content(final) + finish() (+ fold + stamp).
// This is what finalize_turn's PRE-settle block runs on the back message.
void settle_message_md_like(maya::StreamingMarkdown& md, const std::string& text) {
    md.set_content(text);
    md.finish();
}

} // namespace

int main() {
    std::printf("reveal_drain_integration_test\n");
    const std::string body(6000, 'x');

    // ── BUGGY ORDER (current finalize_turn): settle_message_md (finish) THEN
    //    request_finalize. finish() already killed live_, so request_finalize
    //    no-ops and the reveal can't glide.
    {
        maya::StreamingMarkdown md;
        make_midglide(md, body);
        const bool live_before = md.is_live();
        const bool gliding_before = md.reveal_in_progress();

        settle_message_md_like(md, body);   // finalize_turn pre-settle block
        md.request_finalize(200);            // finalize_turn idle branch

        check(live_before && gliding_before,
              "precondition: widget was live + mid-glide at StreamFinished");
        // Document the bug: the buggy order leaves the widget NOT live and NOT
        // finalizing — the glide was skipped, the text snapped in one frame.
        const bool buggy_killed = !md.is_live() && !md.is_finalizing();
        std::printf("  buggy order  -> live=%d finalizing=%d in_progress=%d\n",
                    md.is_live(), md.is_finalizing(), md.reveal_in_progress());
        check(buggy_killed,
              "BUG REPRODUCED: settle_message_md(finish) BEFORE "
              "request_finalize kills the reveal — request_finalize no-ops "
              "because !live_, so the typewriter never glides (snaps/jumps)");
    }

    // ── FIXED ORDER (deferred finalize): request_finalize FIRST while still
    //    live, so the ramp arms; the cursor then glides to the edge over the
    //    ramp window and flips live_ off on its own. (settle_message_md, if
    //    needed at all, runs only AFTER the reveal has drained.)
    {
        maya::StreamingMarkdown md;
        make_midglide(md, body);

        md.request_finalize(200);   // arm the glide WHILE still live

        const bool armed = md.is_live() && md.is_finalizing();
        std::printf("  fixed order  -> live=%d finalizing=%d in_progress=%d\n",
                    md.is_live(), md.is_finalizing(), md.reveal_in_progress());
        check(armed,
              "FIX: request_finalize WHILE live arms the 200ms glide ramp — "
              "the widget stays live and finalizing, so the typewriter glides "
              "to the edge and flips live_ off on its own (smooth, no snap)");

        // And it actually completes: drive build() across the ramp window and
        // assert the widget flips live_ off itself (the glide drained).
        const auto t0 = std::chrono::steady_clock::now();
        while (md.is_live() &&
               std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600)) {
            (void)md.build();
        }
        check(!md.is_live(),
              "FIX: the armed glide drains and the widget flips live_ off on "
              "its own within the ramp window (reveal completes cleanly)");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
