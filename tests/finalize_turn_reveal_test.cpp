// finalize_turn_reveal_test — the PRODUCTION-WIRING proof for the long-turn
// reveal "stuck / jumps instead of typing out" fix (commit edefe9e).
//
// reveal_drain_integration_test proves the WIDGET mechanism (request_finalize
// while live arms a glide; finish() first kills it). But it never calls the
// real reducer, so a regression that re-orders or removes the
// `turn_will_go_idle` branch in finalize_turn would slip past it.
//
// This test drives the REAL agentty::app::detail::finalize_turn reducer over a
// REAL Model whose view_cache holds a REAL, live, mid-glide maya reveal widget
// — exactly the state at the instant StreamFinished lands on a long turn. It
// then asserts the contract end-to-end:
//
//   (A) TEXT-ONLY turn → goes idle → finalize_turn must DEFER finish():
//       the widget is left LIVE + FINALIZING (glide armed), pending_settle_
//       freeze is set, and live_tail_reveal_settled() is false (freeze waits).
//       If a regression makes finalize_turn finish() the widget here, the
//       glide can't run and the reveal snaps — this test goes red.
//
//   (B) TURN WITH A PENDING TOOL → does NOT go idle → finalize_turn settles
//       immediately (height-lock path): the widget is finish()ed (not live),
//       no deferred glide. This guards the OTHER half of the branch so a
//       "just always defer" regression that breaks the tool-path height lock
//       also goes red.
//
//   (C) The deferred glide actually DRAINS: after (A), driving build() across
//       the ramp window flips the widget live_ off on its own, and THEN
//       live_tail_reveal_settled() becomes true — the gate meta.cpp's Tick
//       waits on before the real settle+freeze. Proves the handoff completes.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <maya/widget/markdown.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolUse;
using agentty::StopReason;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (cond) std::printf("  ok: %s\n", (msg));                        \
        else {                                                             \
            ++g_failures;                                                  \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                    \
                         __FILE__, __LINE__, (msg));                       \
        }                                                                  \
    } while (0)

// Seed the Model's view_cache with a live, mid-glide reveal widget for the
// back assistant message — the widget state the moment StreamFinished lands
// on a long turn (cursor far behind a long edge, reveal_fx on, live).
static maya::StreamingMarkdown& seed_midglide_widget(Model& m,
                                                     const Message& msg) {
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);
    cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    auto& md = *cache.streaming;
    md.set_reveal_fx(true);
    md.set_content(msg.text);
    md.set_live(true);
    (void)md.build();   // cursor starts at 0, far behind the long edge
    return md;
}

// Build a Model with one settled user msg + one assistant msg whose body is
// already committed to `text` (finalize_turn drains streaming_text→text at
// the top; we pre-commit so we isolate the settle-ordering branch).
static Model make_model_with_assistant(const std::string& body) {
    Model m;
    m.d.current.id = agentty::ThreadId{"finalize_reveal"};
    Message u; u.role = Role::User; u.text = "go";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant; a.text = body;
    m.d.current.messages.push_back(std::move(a));
    return m;
}

// ── (A) text-only turn: finalize_turn must DEFER finish() and arm the glide.
static void test_textonly_defers_finish() {
    std::printf("test_textonly_defers_finish\n");
    const std::string body(6000, 'x');
    Model m = make_model_with_assistant(body);
    auto& md = seed_midglide_widget(m, m.d.current.messages.back());

    CHECK(md.is_live() && md.reveal_in_progress(),
          "precondition: widget live + mid-glide at StreamFinished");

    (void)agentty::app::detail::finalize_turn(m, StopReason::EndTurn);

    // The fix: text-only / idle-bound turn arms request_finalize(200) WHILE
    // live instead of settle_message_md(finish). So the widget must still be
    // live and finalizing — the glide is armed and will ramp the cursor.
    CHECK(md.is_live(),
          "FIX: text-only finalize_turn left the reveal widget LIVE "
          "(did NOT finish() it) — the typewriter can still glide");
    CHECK(md.is_finalizing(),
          "FIX: text-only finalize_turn armed the finalize ramp "
          "(request_finalize while live) — glide is in flight");
    CHECK(m.ui.pending_settle_freeze,
          "FIX: pending_settle_freeze set — real settle+freeze deferred "
          "to meta.cpp Tick once the reveal drains");
    CHECK(!agentty::app::detail::live_tail_reveal_settled(m),
          "FIX: live_tail_reveal_settled() is FALSE while the glide runs "
          "— the freeze must wait, not fire mid-glide");

    // ── (C) the deferred glide DRAINS on its own, then the gate opens.
    const auto t0 = std::chrono::steady_clock::now();
    while (md.is_live() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(800)) {
        (void)md.build();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    CHECK(!md.is_live(),
          "FIX: the armed glide drained — widget flipped live_ off on its "
          "own within the ramp window (reveal completed cleanly, no snap)");
    CHECK(agentty::app::detail::live_tail_reveal_settled(m),
          "FIX: live_tail_reveal_settled() becomes TRUE after the glide "
          "drains — meta.cpp Tick can now do the byte-identical freeze");
}

// ── (B) turn with a pending tool: NOT idle → settle immediately (finish()).
static void test_toolturn_settles_immediately() {
    std::printf("test_toolturn_settles_immediately\n");
    const std::string body(6000, 'y');
    Model m = make_model_with_assistant(body);
    // Attach a pending tool call → turn will NOT go idle.
    ToolUse tc;
    tc.id   = agentty::ToolCallId{"t1"};
    tc.name = agentty::ToolName{"read"};
    tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
    m.d.current.messages.back().tool_calls.push_back(std::move(tc));
    auto& md = seed_midglide_widget(m, m.d.current.messages.back());

    CHECK(md.is_live() && md.reveal_in_progress(),
          "precondition: widget live + mid-glide at StreamFinished (tool turn)");

    (void)agentty::app::detail::finalize_turn(m, StopReason::ToolUse);

    // Tool path keeps the height-lock discipline: settle_message_md NOW,
    // which finish()es the widget (not live anymore). No deferred glide —
    // this is the half of the branch that must NOT defer. (We assert on the
    // widget state rather than pending_settle_freeze: this synthetic model
    // has no active phase, so the idle-freeze block's is_idle() gate is an
    // orthogonal axis; the load-bearing contract for the tool path is that
    // the widget was finish()ed immediately, not glided.)
    CHECK(!md.is_live(),
          "tool turn: finalize_turn settled (finish()ed) the widget "
          "immediately — height locked before the next view(), as designed");
    CHECK(!md.is_finalizing(),
          "tool turn: widget is settled, not mid-glide (no deferred reveal "
          "ramp on the tool/continuation path)");
}

int main() {
    std::printf("finalize_turn_reveal_test\n");
    // finalize_turn calls deps().save_thread(); install a no-op stub.
    agentty::app::install_deps(agentty::app::Deps{
        .save_thread = [](const agentty::Thread&) {},
    });
    test_textonly_defers_finish();
    test_toolturn_settles_immediately();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
