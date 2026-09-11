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
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdlib>

namespace pn = agentty::ui::panel;

using namespace agentty;

namespace {

// A model with the stats viewer open and a body taller than its viewport,
// so there is somewhere to scroll TO. max_y is what a paint would have
// published; the reducer must respect it without being able to recompute
// it.
Model with_stats_open(int max_y) {
    Model m;
    // A thread with enough turns to fill the panel and overflow it.
    //
    // An EMPTY model was enough for the offset tests above — they assert on
    // the reducer's arithmetic, which does not care what is on screen. It is
    // not enough for the height tests below: a panel with almost nothing in
    // it is shorter than its own viewport, so its height is pinned by the
    // content and cannot respond to scrolling at all. Such a fixture reports
    // "the height never changes" for the one reason that makes the assertion
    // worthless, and a deliberately injected bug walked straight past it.
    for (int i = 0; i < 60; ++i) {
        Message u;
        u.role = Role::User;
        u.text = "a question about the code";
        m.d.current.messages.push_back(std::move(u));

        Message a;
        a.role         = Role::Assistant;
        a.served_model = ModelId{"claude-sonnet-4-5"};
        a.text         = "an answer with enough prose to occupy a row or two";
        Message::Telemetry t;
        t.input_tokens   = 7400;
        t.output_tokens  = 1900;
        t.cache_read     = 52000;
        t.ttft_ms        = 1100;
        t.stream_ms      = 18000;
        a.telemetry = t;
        m.d.current.messages.push_back(std::move(a));
    }
    auto [opened, _] = app::update(std::move(m), Msg{OpenStats{}});
    // The offset lives ON the panel now, so the bound does too.
    opened.ui.panel.get<pn::Stats>()->scroll.max_y = max_y;
    // Model is move-only (it owns the panel slot and the scroll state), so
    // the local has to be moved out rather than copied.
    return std::move(opened);
}

// The panel-owned offset, or -1 when the viewer is closed.
int scroll_y(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Stats>();
    return o ? o->scroll.y : -1;
}

void set_max_y(Model& m, int v) {
    if (auto* o = m.ui.panel.get<pn::Stats>()) o->scroll.max_y = v;
}

int scroll_after(Model m, std::initializer_list<Msg> msgs) {
    for (const auto& msg : msgs) {
        auto [next, _] = app::update(std::move(m), msg);
        m = std::move(next);
    }
    return scroll_y(m);
}

// Painted height of the stats frame: first inked row to last.
//
// Painted rather than measured because the question these tests ask is
// about what the READER sees. A viewport number that stays constant while
// the drawn box changes size is exactly the disagreement worth catching.
[[nodiscard]] int frame_height(const Model& m) {
    setenv("COLUMNS", "100", 1);
    setenv("LINES", "40", 1);
    maya::StylePool pool;
    maya::Canvas canvas(100, 200, &pool);
    canvas.clear();
    maya::render_tree(ui::stats_panel(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);
    int top = -1, bottom = -1;
    for (int y = 0; y < 200; ++y) {
        bool inked = false;
        for (int x = 0; x < 100 && !inked; ++x) {
            const auto ch = canvas.get(x, y).character;
            if (ch && ch != U' ') inked = true;
        }
        if (inked) { if (top < 0) top = y; bottom = y; }
    }
    return top < 0 ? 0 : bottom - top + 1;
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
        CHECK(scroll_y(m) >= 0);
        CHECK(scroll_y(m) <= max_y);

        // A tab switch between scrolls: this is the same-input-batch case,
        // and it must not leave the offset stranded either.
        auto [tabbed, __] = app::update(std::move(m), Msg{StatsTab{+1}});
        m = std::move(tabbed);
        CHECK(scroll_y(m) == 0);
        set_max_y(m, max_y);
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
    CHECK(scroll_y(reopened) == 0);
}

// ── The frame does not move while you read it ──────────────────────────
//
// Every test above asserts on the scroll OFFSET, which is the reducer's
// arithmetic. None of them paints, so none can see the thing a reader
// actually notices: a panel that changes SIZE as its contents move inside
// it. A frame that grows a row when you scroll past the fold, or loses
// one at the bottom, reads as the layout coming apart — and the offset
// was correct the whole time.
//
// This is a real risk in this panel rather than a hypothetical. The body
// is rendered whole and opaque with the viewport clipping it, the
// scrollbar gutter is reserved whether or not a bar is drawn, and the
// panel measures its own content to decide the viewport height. Each of
// those is a place where "how tall am I" could start depending on "where
// am I scrolled to", and the answer must be no.
//
// Painted, not measured, because the question is about what the reader
// sees. The height here is the inked extent of the frame: first row with
// ink to last, which is exactly the box drawn on screen.
TEST_CASE("stats scroll: the frame keeps its height at every offset") {
    // Model is move-only, so each case builds its own from the same fixture
    // rather than copying one aside.
    const int at_top = frame_height(with_stats_open(60));
    CHECK(at_top > 0);

    // Walk the whole range, including past the end: the clamp is what
    // keeps the last screen from being a short one.
    for (int step : {1, 3, 7, 15, 30, 60, 120}) {
        auto [scrolled, _] =
            app::update(with_stats_open(60), Msg{StatsScroll{step}});
        INFO("scrolled by ", step, " -> offset ", scroll_y(scrolled));
        CHECK(frame_height(scrolled) == at_top);
    }

    // And back up, which is a different path through the clamp.
    {
        auto [down, _] = app::update(with_stats_open(60), Msg{StatsScroll{+40}});
        auto [up, __]  = app::update(std::move(down), Msg{StatsScroll{-40}});
        CHECK(frame_height(up) == at_top);
    }

    // The extremes, reached the way the panel reaches them: End and Home
    // are large deltas that the clamp absorbs. Both must still be the same
    // box — the last screen of a document is not a shorter one.
    {
        auto [ended, _] =
            app::update(with_stats_open(60), Msg{StatsScroll{+1000000}});
        CHECK(frame_height(ended) == at_top);
        auto [homed, __] =
            app::update(std::move(ended), Msg{StatsScroll{-1000000}});
        CHECK(frame_height(homed) == at_top);
    }
}

// The same guarantee across TABS.
//
// Switching view is not scrolling, but it is the other way this panel's
// contents change under a frame that must not move. A tall tab followed
// by a short one is exactly the case where a panel sized to its content
// would visibly shrink, and the viewport height is meant to be a property
// of the terminal rather than of the tab.
TEST_CASE("stats scroll: the frame keeps its height across tabs") {
    Model m = with_stats_open(60);
    const int first = frame_height(m);
    CHECK(first > 0);

    for (int i = 0; i < 8; ++i) {
        auto [tabbed, _] = app::update(std::move(m), Msg{StatsTab{+1}});
        m = std::move(tabbed);
        INFO("after ", i + 1, " tab switches");
        CHECK(frame_height(m) == first);
    }
}
