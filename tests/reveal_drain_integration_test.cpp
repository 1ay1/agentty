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
#include <thread>

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

    // ── SCRAMBLE-SETTLE GATE (the frozen-garbage-glyph screenshot) ──
    // The reveal cursor reaching the live edge is NOT enough to drop live_:
    // the scramble tip is still showing random glyphs for ~376 ms after the
    // freshest codepoints arrived (scramble_ms 220 + scramble_len 6 *
    // char_step_ms 26). If live_ flips off while the tail is mid-scramble,
    // the LAST live frame — captured into maya's prev_cells / the host's
    // freeze snapshot — keeps that scramble garbage forever (e.g.
    // "…/agentty.\u03b2\u03b1\u2588=*\u25a1e"). So the widget must stay live until the
    // tail has VISUALLY settled, even after the cursor is at the edge.
    {
        maya::StreamingMarkdown md;
        md.set_reveal_fx(true);
        // Short body whose backlog clears in the first frame — the cursor
        // reaches the edge almost immediately, isolating the scramble gate.
        const std::string tail = "`/home/ayush/agentty/finance`";
        md.set_content(tail);
        md.set_live(true);
        (void)md.build();   // arrival stamp (last_grow) = ~now

        md.request_finalize(50);   // very short ramp: cursor hits edge fast

        // Pump frames for a bit LESS than the scramble-settle window. The
        // cursor reaches the edge but the tail is still scrambling, so the
        // widget MUST stay live (and finalizing) — it may NOT drop live_
        // and strand the scramble.
        const auto s0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - s0 <
               std::chrono::milliseconds(150)) {
            (void)md.build();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        check(md.is_live(),
              "GATE: cursor at edge but scramble still resolving (<376 ms) — "
              "widget STAYS live so the final live frame can't freeze with "
              "scramble garbage on the tail (the screenshot bug)");

        // Now let the scramble fully cool: the widget settles live_ off on
        // its own once the tail is visually clean.
        const auto s1 = std::chrono::steady_clock::now();
        while (md.is_live() &&
               std::chrono::steady_clock::now() - s1 <
                   std::chrono::milliseconds(500)) {
            (void)md.build();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        check(!md.is_live(),
              "GATE: once the scramble has fully resolved (≥376 ms) the "
              "widget flips live_ off — the last live frame is CLEAN text, "
              "so the freeze handoff captures no scramble glyphs");
    }

    // ── SCRAMBLE-SETTLE GATE, LONG MESSAGE / FAST RAMP (the ACTUAL bug) ──
    // This is the case the earlier last_grow_ms_-based gate FAILED to catch
    // and the screenshot kept reproducing: a long message whose BYTES all
    // arrived a while ago (last_grow_ms_ is stale), but whose reveal cursor
    // only just swept the tail during the finalize ramp. The scramble is
    // anchored to the CURSOR, so the freshly-swept tail glyphs are
    // mid-scramble RIGHT NOW — even though no byte has arrived in >376 ms.
    //
    // A gate keyed on (now - last_grow_ms_) sees a stale arrival stamp,
    // declares the tail "settled", and drops live_ the instant the cursor
    // lands — freezing scramble garbage on the tail. The CORRECT gate is
    // keyed on (now - reveal_edge_reached_ms_): the clock starts only when
    // the cursor reaches the edge, so the just-swept tail gets its full
    // scramble window regardless of how old the bytes are.
    {
        maya::StreamingMarkdown md;
        md.set_reveal_fx(true);
        // Long body so the reveal cursor has a big backlog to sweep: the
        // ramp drives it to the edge over the ramp window, NOT instantly.
        const std::string body(8000, 'y');
        md.set_content(body);
        md.set_live(true);
        (void)md.build();   // last_grow stamp = now; cursor starts at 0

        // Let the byte-arrival stamp go STALE without advancing the cursor
        // much: sleep past the 376 ms scramble window. We must keep the
        // cursor well behind the edge, so do NOT pump frames here — a single
        // build at the end of the sleep advances the cursor only one frame's
        // worth, leaving the bulk of the backlog for the ramp to sweep.
        std::this_thread::sleep_for(std::chrono::milliseconds(450));

        // Now arm a fast ramp. The cursor sweeps the whole remaining backlog
        // to the edge within ~80 ms — the tail it exposes is brand-new
        // scramble, but last_grow_ms_ is now ~450 ms in the past.
        md.request_finalize(80);

        // Pump frames for less than the scramble-settle window measured FROM
        // THE EDGE. With the stale-arrival gate this assertion FAILS (the
        // widget already dropped live_ on the frame the cursor landed). With
        // the edge-reached gate it holds: the swept tail is still resolving.
        bool dropped_while_scrambling = false;
        const auto l0 = std::chrono::steady_clock::now();
        bool reached_edge = false;
        while (std::chrono::steady_clock::now() - l0 <
               std::chrono::milliseconds(260)) {
            (void)md.build();
            if (!md.reveal_in_progress()) reached_edge = true;
            // Once the cursor is at the edge, the widget must NOT yet have
            // dropped live_ — the just-swept tail is still mid-scramble.
            if (reached_edge && !md.is_live()) {
                dropped_while_scrambling = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        check(reached_edge,
              "LONG/FAST precondition: the ramp swept the cursor to the live "
              "edge within the test window");
        check(!dropped_while_scrambling,
              "GATE(long/fast): widget did NOT drop live_ the moment the "
              "cursor swept the tail — the cursor-edge clock (not the stale "
              "byte-arrival stamp) gives the just-swept tail its scramble "
              "window. This is the screenshot bug the last_grow_ms_ gate "
              "missed.");

        // And it DOES eventually settle once the swept tail cools.
        const auto l1 = std::chrono::steady_clock::now();
        while (md.is_live() &&
               std::chrono::steady_clock::now() - l1 <
                   std::chrono::milliseconds(500)) {
            (void)md.build();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        check(!md.is_live(),
              "GATE(long/fast): once the swept tail has resolved the widget "
              "settles live_ off — clean final frame, no frozen garbage");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
