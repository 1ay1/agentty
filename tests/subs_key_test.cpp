// subs_key must cover everything subscribe() reads AND everything its
// routers CAPTURE.
//
// jaal's own debug check (kernel::subs_key_covers_subscribe) compares
// SOURCES only — it re-runs subscribe() and asks whether the running set of
// timers/streams would differ. A stale ROUTER CAPTURE is invisible to it: the
// source set is identical, but the closure holds last frame's copy. That is
// the half core/program.hpp warns about, and it is the half that produces
// "the key I just pressed did the previous frame's thing".
//
// So test it the only way that catches captures: mutate one field, and assert
// the key noticed. Each case below names a field the key router closes over.
#include "agtest.hpp"

#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/appearance.hpp"

using namespace agentty;

namespace {

// The key must DIFFER once `mutate` has been applied.
template <class F>
void changes(const char* what, F mutate) {
    Model m;
    const auto before = app::subs_key(m);
    mutate(m);
    const auto after = app::subs_key(m);
    CHECK_MESSAGE(!(before == after),
                  what << ": subscribe() reads this, so subs_key must change "
                          "with it — otherwise the router keeps a stale copy");
}

} // namespace

TEST_CASE("subs_key: stable when nothing subscribe() reads has changed") {
    Model m;
    CHECK(app::subs_key(m) == app::subs_key(m));

    // A field subscribe() does NOT read must not churn the key — that is the
    // whole point of having one.
    const auto before = app::subs_key(m);
    m.s.status = "a status line";
    CHECK(before == app::subs_key(m));
}

TEST_CASE("subs_key: covers every field the key router captures") {
    // ComposerKeyState — the router branches on all five.
    changes("composer text emptiness", [](Model& m) { m.ui.composer.text = "hi"; });
    changes("queued messages",         [](Model& m) { m.ui.composer.queued.push_back({}); });

    // has_history — decides whether ↑ recalls. Needs a NON-EMPTY user turn.
    changes("user history", [](Model& m) {
        Message msg; msg.role = Role::User; msg.text = "hello";
        m.d.current.messages.push_back(std::move(msg));
    });

    // The turn gates. animation_demand starts/stops a real timer.
    changes("turn active", [](Model& m) {
        m.s.phase = phase::Streaming{phase::Active{}};
    });

    // Which overlay owns the keyboard.
    changes("active panel", [](Model& m) {
        m.ui.panel.descend(ui::panel::Appearance{});
    });
}

TEST_CASE("subs_key: covers the pane sub-modes that change routing") {
    // A form pane's MODE decides whether a printable key inserts text or
    // navigates. Same panel Kind either way, so only the mode bits catch it.
    Model m;
    m.ui.panel.descend(ui::panel::Appearance{});
    auto* o = m.ui.panel.get<ui::panel::Appearance>();
    REQUIRE(o != nullptr);

    const auto browsing = app::subs_key(m);

    // Entering the theme picker re-routes every key.
    o->pane.picking = true;
    CHECK_MESSAGE(!(browsing == app::subs_key(m)),
                  "appearance picking mode changes key routing");
}
