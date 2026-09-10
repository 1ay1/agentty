// stats_scroll_test — the stats viewer's scroll offset must never leave
// the range its own invariant claims.
//
// The viewer is read-only, so ↑/↓ act on the VIEWPORT rather than on a
// cursor, and the offset lives on Model::UI (it outlives the panel) while
// its bound, max_y, is owned by the renderer's writeback and only becomes
// true after a paint.
//
// That split is the whole hazard. The reducer runs BEFORE the frame that
// will publish the new bound, so anything it computes by hand is computed
// against last frame's geometry. The old code did:
//
//     y += delta;  if (y < 0) y = 0;      // lower bound only
//
// which let End (nav synthesises move(+1000000) when a panel wires `move`
// but no `jump`) store y = 1000000 and leave it in the model until a paint
// clamped it. Self-correcting on screen, and still wrong: the model held a
// value its own contract forbids, and any reducer reading y before that
// paint -- a StatsScroll arriving in the same input batch as a StatsTab,
// which a fast terminal delivers in ONE read -- read a number that was
// never legal.
//
// These pin the contract, not the arithmetic: after ANY sequence of
// messages, 0 <= y <= max_y.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace pn = agentty::ui::panel;

using namespace agentty;

namespace {

// A model with the stats viewer open and a body taller than its viewport,
// so there is somewhere to scroll TO. max_y is what a paint would have
// published; the reducer must respect it without being able to recompute
// it.
Model with_stats_open(int max_y) {
    Model m;
    auto [opened, _] = app::update(std::move(m), Msg{OpenStats{}});
    opened.ui.stats_scroll.max_y = max_y;
    return opened;
}

int scroll_after(Model m, std::initializer_list<Msg> msgs) {
    for (const auto& msg : msgs) {
        auto [next, _] = app::update(std::move(m), msg);
        m = std::move(next);
    }
    return m.ui.stats_scroll.y;
}

}  // namespace

TEST_CASE("stats scroll: End settles at the bottom, not past it") {
    // nav turns End into move(+1000000) for any panel that wires `move`
    // without a `jump` factory -- which the stats viewer does, because it
    // has no cursor to jump. The reducer has to treat that as "as far as
    // possible", not store it.
    const int max_y = 12;
    CHECK(scroll_after(with_stats_open(max_y), {Msg{StatsScroll{+1000000}}})
          == max_y);
}

TEST_CASE("stats scroll: Home settles at the top, not before it") {
    CHECK(scroll_after(with_stats_open(12), {Msg{StatsScroll{+5}},
                                             Msg{StatsScroll{-1000000}}})
          == 0);
}

TEST_CASE("stats scroll: the offset stays in range through any sequence") {
    // The invariant itself, driven with the deltas nav can actually
    // produce: ±1 (arrows), ±10 (PgUp/PgDn via page_step), ±1000000
    // (Home/End), interleaved with tab switches.
    const int max_y = 9;
    Model m = with_stats_open(max_y);

    const int deltas[] = {+1, +10, -1, +1000000, -10, -1000000, +1, +10, +10};
    int i = 0;
    for (int d : deltas) {
        auto [next, _] = app::update(std::move(m), Msg{StatsScroll{d}});
        m = std::move(next);
        INFO("after delta[", i++, "] = ", d);
        CHECK(m.ui.stats_scroll.y >= 0);
        CHECK(m.ui.stats_scroll.y <= max_y);

        // A tab switch between scrolls: this is the same-input-batch case,
        // and it must not leave the offset stranded either.
        auto [tabbed, __] = app::update(std::move(m), Msg{StatsTab{+1}});
        m = std::move(tabbed);
        CHECK(m.ui.stats_scroll.y == 0);
        m.ui.stats_scroll.max_y = max_y;
    }
}

TEST_CASE("stats scroll: switching tabs returns to the top") {
    // A short tab opened at a tall tab's offset shows a blank body with no
    // visible reason for it.
    Model m = with_stats_open(20);
    CHECK(scroll_after(std::move(m), {Msg{StatsScroll{+15}},
                                      Msg{StatsTab{+1}}}) == 0);
}

TEST_CASE("stats scroll: reopening the viewer starts at the top") {
    // The panel is fresh on every open, but the SCROLL lives on Model::UI
    // and outlives it -- so without an explicit reset a reopen restored
    // the offset from the last close, against a different tab and a max_y
    // describing neither.
    Model m = with_stats_open(20);
    auto [scrolled, _]  = app::update(std::move(m), Msg{StatsScroll{+15}});
    auto [closed, __]   = app::update(std::move(scrolled), Msg{CloseStats{}});
    auto [reopened, ___] = app::update(std::move(closed), Msg{OpenStats{}});
    CHECK(reopened.ui.stats_scroll.y == 0);
}
