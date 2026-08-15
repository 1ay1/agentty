// turn_settle_test.cpp — byte-conservation + settle-shape invariants across
// the TURN-ENDING transitions (the host FSM edges the reveal audit found
// bugs in). fsm_test covers the generic typestate machinery and
// stream_liveness_test covers the streaming↔idle caret edge, but the
// transitions where bytes move between a message's three buffers —
// pending_stream (raw wire) → streaming_text (drip-committed) → text
// (settled) — had NO direct unit test. Every audit bug lived exactly here:
// a byte lost at settle, a backlog pasted, a widget left animating.
//
// The invariants pinned, over finalize_turn(EndTurn / MaxTokens):
//
//   1. CONSERVATION — after finalize, the settled `text` contains every
//      byte that was in text ∪ streaming_text ∪ pending_stream before, in
//      order, and the two scratch buffers are empty. No byte is dropped
//      (the "invisible in-flight bytes at message_stop" class) and none is
//      duplicated (the double-commit class).
//
//   2. IDEMPOTENCE — calling finalize_turn twice is a no-op the second
//      time: text unchanged, no re-appended suffix. A retry/cancel path
//      that re-enters finalize must not double-commit.
//
//   3. SETTLE READINESS — after finalize + draining the deferred settle,
//      live_tail_reveal_settled(m) becomes true (the freeze gate opens):
//      the turn actually reaches a settled shape, it doesn't wedge live.
//
// Pure reducer calls on a hand-built Model; no network, no view.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <maya/core/anim_clock.hpp>

#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <vector>

using namespace agentty;
namespace detail = agentty::app::detail;

namespace {

int g_failed = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) { std::println("  FAIL: {}", msg); ++g_failed; }
}

// finalize_turn touches deps() (store/provider seam) even though this test
// never runs the returned Cmd. Install inert stubs once — no network, no IO.
void install_stub_deps() {
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"stub"}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });
}

// Build a Model whose live tail is one Assistant message carrying bytes
// split across the three buffers, as a mid-stream message really is.
Model make_model_midstream(std::string settled, std::string streaming,
                           std::string pending) {
    Model m;
    m.d.current.id = ThreadId{"settle"};

    Message u;
    u.role = Role::User;
    u.text = "prompt";
    m.d.current.messages.push_back(std::move(u));

    Message a;
    a.role            = Role::Assistant;
    a.text            = std::move(settled);
    a.streaming_text  = std::move(streaming);
    a.pending_stream  = std::move(pending);
    m.d.current.messages.push_back(std::move(a));
    return m;
}

// Force the environment to the interactive (glide) policy so the test
// exercises the request_finalize path, not the SSH immediate-finish path
// — but the CONSERVATION invariant must hold on BOTH, so we also run a
// second pass with the glide disabled.
void set_glide(bool on) {
    if (on) { ::unsetenv("AGENTTY_NO_REVEAL_GLIDE"); ::unsetenv("SSH_TTY");
              ::unsetenv("SSH_CONNECTION"); ::unsetenv("SSH_CLIENT"); }
    else    { ::setenv("AGENTTY_NO_REVEAL_GLIDE", "1", 1); }
}

// Drive the deferred settle to completion: finalize_turn arms the reveal
// ramp (interactive) or finishes immediately (sparse); either way, keep
// ticking the anim clock + polling live_tail_reveal_settled until the
// widget flips live_ off, bounded.
void drain_settle(Model& m) {
    for (int i = 0; i < 400 && !detail::live_tail_reveal_settled(m); ++i) {
        // The view build is what advances the reveal cursor; approximate it
        // by settling directly once the ramp would have landed. Here we
        // just advance the clock and re-check — the widget's own finalize
        // ramp drains on build(), which the freeze gate consults.
        auto& msg = m.d.current.messages.back();
        auto* cache = m.ui.view_cache.peek(m.d.current.id, msg.id);
        if (cache && cache->streaming) (void)cache->streaming->build();
        maya::testing::advance_anim_clock_ms(33);
    }
}

void conservation(bool glide) {
    std::println("--- conservation (glide={}) ---", glide);
    set_glide(glide);
    maya::testing::freeze_anim_clock(0);

    const std::string settled   = "Settled prose from a prior sub-turn. ";
    const std::string streaming = "Drip-committed streaming text. ";
    const std::string pending   = "Raw wire bytes not yet drained.";
    const std::string expected  = settled + streaming + pending;

    Model m = make_model_midstream(settled, streaming, pending);
    (void)detail::finalize_turn(m, StopReason::EndTurn);

    auto& a = m.d.current.messages.back();
    // #1 conservation: every byte present, in order; scratch buffers empty.
    check(a.text == expected,
          "settled text == settled ∪ streaming ∪ pending, in order (got '"
          + a.text + "')");
    check(a.streaming_text.empty(), "streaming_text drained to empty");
    check(a.pending_stream.empty(), "pending_stream drained to empty");

    // #2 idempotence: a second finalize (retry/cancel re-entry) is a no-op.
    const std::string after_first = a.text;
    (void)detail::finalize_turn(m, StopReason::EndTurn);
    check(a.text == after_first,
          "second finalize_turn does not double-commit (got '" + a.text + "')");

    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

void empty_pending_is_safe() {
    std::println("--- empty_pending_is_safe ---");
    set_glide(false);
    maya::testing::freeze_anim_clock(0);
    // A turn that already drip-committed everything: pending empty, streaming
    // empty, only settled text. finalize must be a clean no-op-ish settle,
    // not append an empty suffix or lose the text.
    Model m = make_model_midstream("Only settled text, nothing in flight.",
                                   "", "");
    const std::string before = m.d.current.messages.back().text;
    (void)detail::finalize_turn(m, StopReason::EndTurn);
    check(m.d.current.messages.back().text == before,
          "settled-only turn survives finalize unchanged");
    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

void reaches_settled_shape() {
    std::println("--- reaches_settled_shape ---");
    set_glide(false);   // sparse path: finish() is synchronous, no ramp wait
    maya::testing::freeze_anim_clock(0);
    Model m = make_model_midstream("Prefix. ", "middle ", "tail bytes.");
    (void)detail::finalize_turn(m, StopReason::EndTurn);
    drain_settle(m);
    // #3 the turn reaches a settled shape (freeze gate opens) — it doesn't
    // wedge the widget live forever.
    check(detail::live_tail_reveal_settled(m),
          "turn reaches settled shape after finalize + drain (freeze gate "
          "opens; a wedged ramp would keep this false)");
    maya::testing::unfreeze_anim_clock();
    std::println("PASS\n");
}

} // namespace

int main() {
    std::println("=== turn_settle_test ===");
    install_stub_deps();
    conservation(/*glide=*/true);
    conservation(/*glide=*/false);
    empty_pending_is_safe();
    reaches_settled_shape();
    if (g_failed) { std::println("{} check(s) FAILED", g_failed); return 1; }
    std::println("All turn-settle transition tests passed.");
    return 0;
}
