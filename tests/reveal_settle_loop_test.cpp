// reveal_settle_loop_test — REPRODUCES the screenshot bug: a SHORT assistant
// reply settles but its reveal overlay stays frozen showing scramble garbage
// (`/home/$0█*&a`) forever — the widget never flips live_ off, so the
// scramble/gradient/caret decoration is stuck on screen at idle.
//
// Why the earlier proofs missed it: they called build() in a TIGHT LOOP
// (every iteration, unconditionally). The real host does NOT — maya's run
// loop only re-renders when something requested an animation frame
// (request_animation_frame → animation_requested_) OR the visual hash
// changed. If a settled-but-still-live widget ever fails to re-arm a frame
// while reveal_cp_ < total_cp (cursor not yet at the edge) AND no ramp is
// in flight, the loop goes to sleep and the cursor freezes mid-reveal —
// the scramble overlay is stranded on screen exactly as in the screenshot.
//
// This test drives the REAL host render path (ui::thread_config →
// maya::Thread::build → cached_markdown_for) and gates every frame on the
// PREVIOUS frame's animation_requested_ flag — i.e. it models the run loop
// faithfully. The invariant: while the widget is still live_ (overlay
// showing), the render MUST keep requesting frames until it settles. If a
// frame renders the overlay but does NOT request the next one while live_,
// the loop deadlocks and the test FAILS — catching the frozen scramble.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <maya/app/app.hpp>          // maya::detail::animation_requested_
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/thread.hpp>
#include <maya/widget/markdown.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::StopReason;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond, msg)                                                   \
    do { ++g_checks;                                                       \
        if (cond) std::printf("  ok: %s\n", (msg));                        \
        else { ++g_failures;                                               \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                    \
                         __FILE__, __LINE__, (msg)); }                     \
    } while (0)

// Render the conversation region once; return whether ANY widget requested
// an animation frame during the build (mirrors the run loop's wake gate).
static bool render_frame_requests_anim(const Model& m,
                                       int w = 100, int h = 40) {
    maya::detail::animation_requested_ = false;
    auto root = maya::Thread{agentty::ui::thread_config(m)}.build();
    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);
    return maya::detail::animation_requested_;
}

// Is the back assistant message's reveal widget still live (overlay showing)?
static bool back_widget_live(const Model& m) {
    const auto& msg = m.d.current.messages.back();
    const auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);
    return cache.streaming && cache.streaming->is_live();
}

// Drive a FAITHFUL run loop that models BOTH gates maya's run<P> applies:
//   • the visual_hash skip gate (skip render when the hash is unchanged), and
//   • the request_animation_frame override (a RAF from the PRIOR rendered
//     frame forces this frame to render even if the hash didn't move).
// This is the combination the earlier (passing) version missed: it rendered
// every iteration unconditionally, so it could never observe a hash-static
// frame breaking the RAF chain. Returns true if the widget reached !live_
// (settled cleanly); false if the loop quiesced while still live (= the
// frozen-scramble bug: no render scheduled, cursor stuck, overlay stranded).
static bool run_loop_until_settled(Model& m, int max_ms = 1500) {
    using agentty::app::AgenttyApp;
    const auto t0 = std::chrono::steady_clock::now();

    bool last_hash_valid = false;
    std::uint64_t last_hash = 0;
    bool raf_from_prev_frame = true;   // a state change triggered the first paint

    while (back_widget_live(m)) {
        if (std::chrono::steady_clock::now() - t0 >
            std::chrono::milliseconds(max_ms))
            return false;   // timed out still live = freeze

        const std::uint64_t h = AgenttyApp::visual_hash(m);
        bool skip = last_hash_valid && h == last_hash;
        // RAF override: a frame request issued during the PREVIOUS rendered
        // frame forces this one even if the hash is static.
        if (raf_from_prev_frame) skip = false;
        last_hash = h; last_hash_valid = true;

        if (skip) {
            // maya's loop clears next_frame_at on a skipped frame: with no
            // RAF pending and a static hash, NOTHING reschedules a paint.
            // The loop would sleep on the idle poll — and the widget is
            // still live, so the scramble overlay is stranded. THE BUG.
            return false;
        }

        // Render: this is where build() runs, advances the reveal cursor,
        // and (while still animating) re-issues request_animation_frame().
        raf_from_prev_frame = render_frame_requests_anim(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return true;            // widget flipped live_ off — overlay cleared
}

// Build a model with one user msg + a SHORT assistant reply mid-stream,
// then finalize it (text-only → deferred glide path).
static Model make_short_reply_model(const std::string& body) {
    Model m;
    m.d.current.id = agentty::ThreadId{"reveal_settle"};
    Message u; u.role = Role::User; u.text = "cwd?";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);
    Message a; a.role = Role::Assistant;
    a.streaming_text = body;   // still in the smoothing/streaming buffer
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    return m;
}

// ── The repro: stream a short reply, render a few live frames (so the
//    reveal widget exists, is live, mid-scramble), then StreamFinished →
//    finalize_turn (text-only → deferred glide), then run the faithful
//    loop. The widget MUST settle (live_ off) — no frozen scramble.
static void test_short_reply_settles_no_frozen_scramble() {
    std::printf("test_short_reply_settles_no_frozen_scramble\n");
    // A short, code-spanish body like the screenshot ("/home/$0...").
    const std::string body = "`/home/ayush/agentty/finance`";
    Model m = make_short_reply_model(body);

    // A few live frames so the widget is created, live, and mid-reveal.
    for (int i = 0; i < 3; ++i) {
        (void)render_frame_requests_anim(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    CHECK(back_widget_live(m),
          "precondition: widget live + animating during the stream");

    // StreamFinished: drain streaming_text→text + run the REAL reducer.
    // This is the text-only/idle path: finalize_turn arms request_finalize
    // WHILE live and sets pending_settle_freeze (the deferred-glide window).
    (void)agentty::app::detail::finalize_turn(m, StopReason::EndTurn);
    // finalize_turn moved the model toward idle; mirror the host's phase
    // transition so is_streaming() is false now (the settled regime).
    m.s.phase = agentty::phase::Idle{};

    CHECK(m.ui.pending_settle_freeze,
          "precondition: in the deferred-glide window (pending_settle_freeze)");

    // ── THE LOAD-BEARING INVARIANT (the screenshot bug's root cause) ──
    // The reveal glide advances ONLY inside build(), which the host runs
    // ONLY when the visual hash moves OR a RAF is pending. The RAF chain
    // is self-sustaining ONLY as long as the hash also keeps the render
    // gate open between scheduled frames. After StreamFinished the phase
    // is Idle and streaming_text is drained, so the streaming buckets go
    // quiet — if visual_hash then falls to the caret-blink PARITY bucket
    // (one flip / 265 ms) the render gate STARVES the 16 ms glide: build()
    // stops being called, live_ never flips off, and the scramble overlay
    // is stranded on screen (the frozen-garbage-glyph screenshot).
    //
    // Assert directly that the hash advances across a 16 ms window for the
    // WHOLE drain. This is what `draining_reveal = pending_settle_freeze`
    // (program.hpp) guarantees. Revert that term and this loop catches a
    // window where the hash is static while pending_settle_freeze holds.
    using agentty::app::AgenttyApp;
    bool hash_stalled_during_drain = false;
    for (int i = 0; i < 40 && m.ui.pending_settle_freeze; ++i) {
        const std::uint64_t h0 = AgenttyApp::visual_hash(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        const std::uint64_t h1 = AgenttyApp::visual_hash(m);
        if (h0 == h1) { hash_stalled_during_drain = true; break; }
        // Drive a render so the glide cursor advances and the model's
        // pending_settle_freeze eventually clears (mirrors the host).
        (void)render_frame_requests_anim(m);
        // The host's meta.cpp Tick clears pending_settle_freeze once the
        // reveal drains; emulate that gate here.
        if (agentty::app::detail::live_tail_reveal_settled(m))
            m.ui.pending_settle_freeze = false;
    }
    CHECK(!hash_stalled_during_drain,
          "FIX: visual_hash advances every 16 ms for the WHOLE deferred-"
          "glide window (pending_settle_freeze) — the render gate never "
          "starves the reveal cursor. If this fails, the hash went static "
          "mid-drain and the scramble overlay freezes on screen "
          "(the screenshot bug).");

    const bool settled = run_loop_until_settled(m);
    CHECK(settled,
          "FIX: after StreamFinished the reveal overlay glides to the edge "
          "and the widget flips live_ off — NO frozen scramble at idle "
          "(the screenshot bug). If this fails the run loop went to sleep "
          "while the widget was still live, stranding the scramble glyphs.");

    CHECK(!back_widget_live(m),
          "FIX: widget is settled (live_ off) — overlay cleared to clean "
          "static text");
}

int main() {
    std::printf("reveal_settle_loop_test\n");
    agentty::app::install_deps(agentty::app::Deps{
        .save_thread = [](const agentty::Thread&) {},
    });
    test_short_reply_settles_no_frozen_scramble();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
