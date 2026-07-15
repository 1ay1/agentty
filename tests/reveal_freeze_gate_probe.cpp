// reveal_freeze_gate_probe — PROVES the "symptom 2" freeze window and
// that program.hpp::visual_hash closes it.
//
// SYMPTOM 2 (the one being fixed): a model reply reveals part-way, then
// FREEZES for a noticeable time, then the whole tail appears at once —
// most visible over SSH / any link that goes quiet mid-reveal.
//
// MECHANISM (verified against code, not theory):
//   Three parties gate whether a frame paints while a reveal animates:
//     • the Tick subscription   (subscribe.cpp) — armed while
//         !detail::live_tail_reveal_settled(m)
//     • RAF from cached_markdown_for (turn.cpp) — armed while the widget
//         is_live() / reveal_in_progress() / is_finalizing()
//     • Program::visual_hash (program.hpp) — maya's run loop SKIPS the
//         whole view()+render() pair when this hash is unchanged
//         (maya/app/app.hpp).
//   The first two keep frames REQUESTED off live_tail_reveal_settled().
//   The hash's fast (16 ms) reveal bucket used to fire ONLY when
//   streaming_text/pending_stream were non-empty (`revealing_text`) or a
//   settle-freeze was pending. But there is a real window where NEITHER
//   holds: the wire finished the text block, its bytes were committed
//   from streaming_text into `text`, phase left Streaming — yet the
//   reveal cursor is STILL gliding to the edge (is_finalizing /
//   reveal_in_progress). There the hash fell to the 265 ms caret-blink
//   parity bucket, so every armed Tick/RAF frame was gated AWAY, view()
//   never ran, and the typewriter froze until the next caret flip / key.
//
// THIS PROBE constructs exactly that window with the REAL widget and the
// REAL AgenttyApp::visual_hash, and asserts:
//   (A) In the freeze window, live_tail_reveal_settled(m) == false
//       (so the Tick/RAF frame sources ARE armed — the frames exist).
//   (B) In the freeze window, visual_hash advances across a 16 ms
//       wall-clock step (the fix: those armed frames now actually
//       render — continuous reveal, no freeze).
//   (C) The pre-fix gate would NOT have advanced here — computed from
//       the same model via the exact old predicate — so the window was
//       genuinely dead before, not already covered by another axis.
//   (D) Once the widget finishes (settled), live_tail_reveal_settled is
//       true AND the hash is stable across a 16 ms step (no idle-CPU
//       regression: settled turns still gate to zero renders).

#include <chrono>
#include <cstdio>
#include <cstdlib>   // setenv
#include <thread>

#include <maya/widget/markdown.hpp>   // maya::StreamingMarkdown

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::app::AgenttyApp;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                     \
                         __FILE__, __LINE__, (msg));                        \
        }                                                                   \
    } while (0)

// The EXACT pre-fix reveal-drain gate, replicated so we can show the old
// hash would NOT have advanced in this window. Mirrors the two terms the
// gate had before the third (live_tail_reveal_settled) term was added.
static bool old_draining_reveal(const Model& m) {
    return m.ui.pending_settle_freeze || m.ui.settle_cooldown_ticks > 0;
}
// The `revealing_text` term (unchanged by the fix) — keys off wire bytes
// still being buffered, which is FALSE in the freeze window.
static bool revealing_text(const Model& m) {
    return m.s.active()
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant
        && (!m.d.current.messages.back().streaming_text.empty()
            || !m.d.current.messages.back().pending_stream.empty());
}

// Build a live-tail model parked in the freeze window: phase Idle,
// streaming_text drained into `text`, but a StreamingMarkdown in the
// view cache is still live_ + finalizing (reveal cursor gliding).
static Model freeze_window_model() {
    Model m;
    m.d.current.id = agentty::ThreadId{"freeze"};

    Message u; u.role = Role::User; u.text = "explain";
    m.d.current.messages.push_back(std::move(u));

    Message a; a.role = Role::Assistant;
    // Bytes fully committed to `text`; NOTHING left in streaming_text /
    // pending_stream (the wire is done, the pacer drained). A long body
    // so the reveal cursor legitimately still has backlog to glide out.
    a.text =
        "Here is a fairly long paragraph of prose that the reveal cursor "
        "will still be gliding across when the wire has already delivered "
        "every byte, which is precisely the window where the old gate let "
        "the typewriter freeze until the next caret blink.";
    m.d.current.messages.push_back(std::move(a));

    // Phase is IDLE — the tell-tale of the window. m.s.active() is false,
    // so `revealing_text` and the spinner/fine-anim buckets are all off.
    m.s.phase = agentty::phase::Idle{};

    // Populate the view cache with a widget mid-reveal: live_ on, content
    // set, then request_finalize so is_finalizing()/reveal_in_progress()
    // are true but the cursor has NOT reached the edge yet.
    auto& cache = m.ui.view_cache.message_md(
        m.d.current.id, m.d.current.messages.back().id);
    cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    cache.streaming->set_reveal_fx(true);
    cache.streaming->set_reveal_pacing(/*floor_cps=*/30.0, /*drain_secs=*/0.8);
    cache.streaming->set_content(m.d.current.messages.back().text);
    cache.streaming->set_live(true);
    (void)cache.streaming->build();          // seat the reveal cursor at 0
    cache.streaming->request_finalize(200);  // arm the glide-to-edge ramp
    (void)cache.streaming->build();          // advance one frame; still mid-glide
    return m;
}

static void test_freeze_window_exists_and_is_now_driven() {
    std::printf("freeze window: armed frames must advance the hash\n");
    Model m = freeze_window_model();

    // (A) The frame SOURCES are armed: the subscription/RAF predicate is
    //     false, so Ticks + RAF are firing. The frames exist — the only
    //     question is whether visual_hash lets them paint.
    const bool settled = agentty::app::detail::live_tail_reveal_settled(m);
    CHECK(!settled,
          "precondition: reveal still in progress (live_tail_reveal_settled "
          "must be false here, else this isn't the freeze window)");

    // Sanity: the OTHER fast-bucket terms are all off in this window, so
    // the reveal bucket is the ONLY thing that can advance the hash fast.
    CHECK(!revealing_text(m),
          "freeze window: revealing_text must be false (no buffered wire "
          "bytes) — otherwise the window is already covered and this probe "
          "proves nothing");
    CHECK(!old_draining_reveal(m),
          "freeze window: old draining_reveal must be false (no pending "
          "settle-freeze) — the pre-fix gate had no fast term here");
    CHECK(!m.s.active(),
          "freeze window: m.s.active() must be false (phase Idle) so the "
          "spinner/fine-anim bucket is off");

    // (C) PRE-FIX: with revealing_text=false and old_draining_reveal=false
    //     and !active(), the hash fell to the 265 ms caret parity bucket.
    //     Across a 16 ms step that parity does NOT flip → hash static →
    //     frame gated away → FREEZE. Demonstrate the parity is unchanged.
    {
        const auto now_ms = []() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        const std::int64_t kBlinkHalfMs = 265;
        const auto p0 = (now_ms() / kBlinkHalfMs) & 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const auto p1 = (now_ms() / kBlinkHalfMs) & 1;
        CHECK(p0 == p1,
              "pre-fix regime: caret parity flipped within 16 ms — pick a "
              "different step; the point is the OLD bucket was ~265 ms so a "
              "16 ms reveal step was gated away");
    }

    // (B) POST-FIX: the real visual_hash must ADVANCE across a 16 ms step
    //     while the reveal is in progress. This is the fix working: the
    //     reveal bucket (16 ms) is now keyed on !live_tail_reveal_settled,
    //     so each armed frame actually renders and the typewriter glides.
    const std::uint64_t h0 = AgenttyApp::visual_hash(m);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const std::uint64_t h1 = AgenttyApp::visual_hash(m);
    CHECK(h0 != h1,
          "FIX BROKEN: visual_hash did NOT advance across a 16 ms step "
          "while a reveal was in progress — the freeze window is still "
          "dead; the typewriter will stall until a caret flip / keypress");
}

static void test_settled_turn_is_hash_stable() {
    std::printf("settled turn: hash must go quiet (no idle-CPU regression)\n");
    Model m = freeze_window_model();

    // Finish the widget: this is what finalize_turn/settle_message_md do.
    // Now live_tail_reveal_settled must be true and the fast bucket off.
    auto& cache = m.ui.view_cache.message_md(
        m.d.current.id, m.d.current.messages.back().id);
    cache.streaming->finish();
    (void)cache.streaming->build();

    const bool settled = agentty::app::detail::live_tail_reveal_settled(m);
    CHECK(settled,
          "after finish(): live_tail_reveal_settled must be true — the "
          "widget dropped live_/reveal/finalize/parse");

    // With the reveal settled AND phase Idle, the hash must be stable
    // across a 16 ms step — a settled thread does ZERO idle renders.
    const std::uint64_t h0 = AgenttyApp::visual_hash(m);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    const std::uint64_t h1 = AgenttyApp::visual_hash(m);
    CHECK(h0 == h1,
          "IDLE REGRESSION: visual_hash advanced across 16 ms on a fully "
          "settled idle turn — the fast reveal bucket is leaking and the "
          "loop will burn CPU at idle");
}

int main() {
    // Pin the sync-output classification so the reveal render bucket is the
    // deterministic 16 ms (60 fps) cadence this probe's ±16 ms step
    // assertions are written against. On a NON-sync terminal the bucket is
    // the tick period (100 ms) — correct behaviour (see program.hpp: 60 fps
    // only floods a progressively-painting terminal like Termux), but a
    // 16 ms step wouldn't reliably cross a 100 ms bucket boundary, making
    // these timing checks flaky. The gate MECHANISM under test (armed reveal
    // frames advance the hash; settled turns go quiet) is identical in both
    // modes — only the period differs — so forcing sync keeps the probe
    // deterministic without weakening what it proves.
    setenv("MAYA_FORCE_SYNC", "1", /*overwrite=*/1);

    std::printf("reveal_freeze_gate_probe\n");
    test_freeze_window_exists_and_is_now_driven();
    test_settled_turn_is_hash_stable();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
