// stats_visual_hash_test — the render gate must SEE the stats viewer.
//
// maya's run loop skips the whole view()+render() pair when
// AgenttyApp::visual_hash is unchanged. That gate is what makes idle cheap,
// and it is also a trapdoor: any state the view reads but the hash does not
// mix produces a model the loop calls "visually identical", so the frame is
// skipped and the change only reaches the screen when some UNRELATED hashed
// axis flips -- the ~265 ms caret-blink parity, or the next keystroke.
//
// The symptom is not "slow". It is "the panel ignored me, then caught up
// when I typed": Esc appears not to work (the panel HAS closed; the screen
// was not told), scrolling registers once per several presses, and tab
// switches land late. The changelog records exactly this for the pickers;
// the stats viewer shipped with the same hole.
//
// It looked intermittent because a tab that FITS its viewport clamps every
// scroll to the same y and is genuinely unchanged -- so only the tall tabs
// misbehaved.
//
// These pin the contract: a model the user changed must not hash equal to
// the one they changed it from.

#include "agtest.hpp"

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace pn = agentty::ui::panel;

using namespace agentty;
using agentty::app::AgenttyApp;

namespace {

// The viewer open on a thread with enough turns that several tabs are
// available and taller than a viewport.
Model open_viewer() {
    Model m;
    Thread t;
    t.id = ThreadId{"vh"};
    for (int i = 0; i < 24; ++i) {
        Message u;
        u.role = Role::User;
        u.text = "ask";
        t.messages.push_back(std::move(u));

        Message a;
        a.role         = Role::Assistant;
        a.served_model = ModelId{"claude-sonnet-4-5"};
        a.text         = "answer";
        Message::Telemetry tel;
        tel.input_tokens   = 900;
        tel.output_tokens  = 300;
        tel.cache_read     = 12000;
        tel.ttft_ms        = 120;
        tel.stream_ms      = 800;
        a.telemetry = tel;
        t.messages.push_back(std::move(a));
    }
    m.d.current = std::move(t);

    auto [opened, _] = app::update(std::move(m), Msg{OpenStats{}});
    // A bound to scroll within: the renderer publishes this after a paint,
    // and the reducer clamps against it.
    opened.ui.stats_scroll.max_y = 20;
    return opened;
}

}  // namespace

TEST_CASE("stats gate: scrolling advances the render hash") {
    Model m = open_viewer();
    const std::uint64_t before = AgenttyApp::visual_hash(m);

    auto [after, _] = app::update(std::move(m), Msg{StatsScroll{+3}});
    REQUIRE(after.ui.stats_scroll.y == 3);      // the model really moved

    CHECK(AgenttyApp::visual_hash(after) != before);
}

TEST_CASE("stats gate: switching tabs advances the render hash") {
    // This one held before the scroll fix too: stats_panel::Open declares
    // the tab in its visual_parts, so the slot walk already covered it.
    // Pinned anyway -- it is half of what the user reported, and the parts
    // list is a thing a future refactor can quietly drop.
    Model m = open_viewer();
    const auto* before_tab = m.ui.panel.get<pn::Stats>();
    REQUIRE(before_tab != nullptr);
    const stats::Tab t0 = before_tab->tab;
    const std::uint64_t before = AgenttyApp::visual_hash(m);

    auto [after, _] = app::update(std::move(m), Msg{StatsTab{+1}});
    const auto* after_tab = after.ui.panel.get<pn::Stats>();
    REQUIRE(after_tab != nullptr);
    REQUIRE(after_tab->tab != t0);              // the tab really changed

    CHECK(AgenttyApp::visual_hash(after) != before);
}

TEST_CASE("stats gate: closing the viewer advances the render hash") {
    // The Esc symptom. Closing already changed the panel variant, so this
    // held before the fix too -- pinned because it is the property the user
    // actually reported, and a future change to the slot walk must not
    // quietly break it.
    Model m = open_viewer();
    const std::uint64_t before = AgenttyApp::visual_hash(m);

    auto [after, _] = app::update(std::move(m), Msg{CloseStats{}});
    CHECK(AgenttyApp::visual_hash(after) != before);
}

TEST_CASE("stats gate: a scroll that changes nothing does not advance it") {
    // The other half of the contract. The gate exists to make idle cheap,
    // so a no-op must stay a no-op: scrolling up at the top is clamped to
    // the same y, and hashing it differently would burn a frame per
    // keypress for no pixels.
    Model m = open_viewer();
    REQUIRE(m.ui.stats_scroll.y == 0);
    const std::uint64_t before = AgenttyApp::visual_hash(m);

    auto [after, _] = app::update(std::move(m), Msg{StatsScroll{-5}});
    REQUIRE(after.ui.stats_scroll.y == 0);      // clamped, nothing moved

    CHECK(AgenttyApp::visual_hash(after) == before);
}
