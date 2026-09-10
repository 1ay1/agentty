// stat_sheet_measure_test — the sheet's reported height must BOUND its
// painted height, at every width.
//
// This is the invariant behind a scrollable panel. maya's Panel measures
// prebuilt body content once, budgets `max_y` from that number, and then
// clips the body to the viewport. So a sheet that reports FEWER rows than
// it paints does not merely look wrong -- the surplus rows become
// unreachable, and max_y reports there is nothing to scroll to. Content
// disappears with no affordance suggesting it ever existed.
//
// The bug this pins: the sheet SPLITS into columns once it is wide enough,
// which roughly halves its height. Measured at the host's wide sentinel it
// reported the split height (9 rows) while the paint pass at 76 columns --
// one column short of a split -- produced 19. The Cache tab silently lost
// its Tokens and Rates sections.
//
// Asserting on the numbers 9 and 19 would pin the fixture, not the
// property. The assertion is the RELATION: reported >= painted, for every
// width a terminal might have.

#include <doctest/doctest.h>

#include "maya/element/builder.hpp"
#include "maya/render/canvas.hpp"
#include "maya/render/renderer.hpp"
#include "maya/style/theme.hpp"
#include "maya/widget/stat_sheet.hpp"

using namespace maya;

namespace {

// A sheet shaped like the Cache tab: a tall figure, then two small tables.
// That shape is what makes the split so much shorter than the single
// column, which is what made the under-report large enough to lose whole
// sections.
Element cache_shaped_sheet() {
    StatSheet s;
    s.indent(1);
    s.reserve_right(7);
    s.columns(2);

    s.heading("input tokens by origin");
    StatDonut d;
    d.segments = {{"hit", 80, Color::green()},
                  {"write", 7, Color::yellow()},
                  {"miss", 13, Color::red()}};
    d.center = "80% hit";
    s.donut(std::move(d));

    s.blank();
    s.heading("Tokens");
    s.entry({.label = "Served from cache", .value = "93k", .share = 1.0});
    s.entry({.label = "Written to cache", .value = "8.2k", .share = 0.09});
    s.entry({.label = "Sent uncached", .value = "16k", .share = 0.17});

    s.blank();
    s.heading("Rates");
    s.entry({.label = "Hit rate", .value = "80%", .share = 0.8});
    s.entry({.label = "Turns using cache", .value = "8", .share = 1.0});
    return s.build();
}

}  // namespace

TEST_CASE("stat sheet: reported height bounds painted height at every width") {
    // 1<<14 is what maya's Panel passes when it measures prebuilt content.
    const int reported = measure_element(cache_shaped_sheet(), 1 << 14)
                             .height.value;
    REQUIRE(reported > 0);

    // Every width from a cramped split pane to an ultrawide terminal. The
    // interesting band is either side of the split threshold, which is
    // exactly where a single sampled width would have missed the bug.
    for (int w = 40; w <= 320; ++w) {
        const int painted = measure_element(cache_shaped_sheet(), w)
                                .height.value;
        INFO("width=", w, " painted=", painted, " reported=", reported);
        CHECK(painted <= reported);
    }
}

TEST_CASE("stat sheet: reported width follows the surface it is given") {
    // The measure() callback answers a question about HEIGHT. It must not
    // smuggle a WIDTH decision into that answer.
    //
    // It used to probe several candidate widths to find the tallest layout
    // and then return the probe width it had landed on, which told the
    // layout engine the sheet's natural width was 40 columns. The engine
    // duly sized it to 40 and left it there -- so on a 300-column terminal
    // every bar was frozen at the same 16 cells with two thirds of the
    // panel blank, and resizing changed nothing at all.
    StatSheet s;
    s.indent(1);
    s.columns(2);
    s.heading("By tool");
    s.entry({.label = "read", .value = "3", .share = 1.0});
    s.entry({.label = "edit", .value = "3", .share = 1.0});
    s.entry({.label = "grep", .value = "2", .share = 0.66});
    const auto el = s.build();

    // Whatever width it is measured at, that is the width it reports.
    for (int w = 40; w <= 300; w += 4) {
        INFO("width=", w);
        CHECK(measure_element(el, w).width.value == w);
    }

    // And the SENTINEL path is the one that actually broke. maya's Panel
    // measures prebuilt content at 1<<14; the callback substitutes a small
    // probe width internally to find the tallest layout, and it must not
    // let that substitution escape into the reported width. Reporting the
    // probe width here is exactly what pinned the sheet to 40 columns
    // forever, so a width sweep that never reaches the sentinel -- like
    // the loop above on its own -- sails straight past the bug.
    for (int sentinel : {1 << 14, 1 << 20}) {
        INFO("sentinel=", sentinel);
        CHECK(measure_element(el, sentinel).width.value == sentinel);
    }
}

TEST_CASE("stat sheet: columns answer vertical pressure, not available width") {
    // The principle: columns exist to relieve VERTICAL pressure. Width is
    // a constraint on splitting (a column too narrow to draw is damage);
    // height is the REASON for it.
    //
    // The sheet used to know only about width, so it split whenever it
    // COULD -- which put a short Session tab into two columns on a
    // phone-sized pane while a genuinely overflowing tab sat in one
    // because a slice came up a few cells short. Both are the same bug
    // seen from opposite ends: the layout was answering a question nobody
    // asked.
    auto make = [](int budget) {
        StatSheet s;
        s.indent(1);
        s.reserve_right(7);
        s.columns(2);
        s.height_budget(budget);
        s.heading("Activity");
        s.entry({.label = "You asked", .value = "8", .share = 0.8});
        s.entry({.label = "Agent replied", .value = "8", .share = 0.8});
        s.entry({.label = "Tool calls", .value = "10", .share = 1.0});
        s.blank();
        s.heading("Time");
        s.entry({.label = "Waiting", .value = "6.0s", .share = 0.1});
        s.entry({.label = "Generating", .value = "59s", .share = 1.0});
        s.entry({.label = "Tools", .value = "8.2s", .share = 0.14});
        s.blank();
        s.heading("Rates");
        s.entry({.label = "Hit rate", .value = "80%", .share = 0.8});
        s.entry({.label = "Turns using cache", .value = "8", .share = 1.0});
        return s.build();
    };

    // Height with splitting forbidden -- the sheet's true single column.
    const int natural = measure_element([] {
        StatSheet s;
        s.indent(1);
        s.reserve_right(7);
        s.columns(1);
        s.heading("Activity");
        s.entry({.label = "You asked", .value = "8", .share = 0.8});
        s.entry({.label = "Agent replied", .value = "8", .share = 0.8});
        s.entry({.label = "Tool calls", .value = "10", .share = 1.0});
        s.blank();
        s.heading("Time");
        s.entry({.label = "Waiting", .value = "6.0s", .share = 0.1});
        s.entry({.label = "Generating", .value = "59s", .share = 1.0});
        s.entry({.label = "Tools", .value = "8.2s", .share = 0.14});
        s.blank();
        s.heading("Rates");
        s.entry({.label = "Hit rate", .value = "80%", .share = 0.8});
        s.entry({.label = "Turns using cache", .value = "8", .share = 1.0});
        return s.build();
    }(), 200).height.value;
    REQUIRE(natural > 4);

    SUBCASE("a sheet that fits its viewport does not split, however wide") {
        // Generous budget: there is no vertical pressure, so no amount of
        // width should buy a second column. This is the case from the bug
        // report -- a short tab in two columns on a narrow phone pane.
        //
        // Compared against a sheet forbidden to split at all. "No budget"
        // is NOT the right baseline: it means the host has no opinion
        // about height, and the sheet then falls back to splitting on
        // width like it always did.
        auto single = [] {
            StatSheet s;
            s.indent(1);
            s.reserve_right(7);
            s.columns(1);
            s.heading("Activity");
            s.entry({.label = "You asked", .value = "8", .share = 0.8});
            s.entry({.label = "Agent replied", .value = "8", .share = 0.8});
            s.entry({.label = "Tool calls", .value = "10", .share = 1.0});
            s.blank();
            s.heading("Time");
            s.entry({.label = "Waiting", .value = "6.0s", .share = 0.1});
            s.entry({.label = "Generating", .value = "59s", .share = 1.0});
            s.entry({.label = "Tools", .value = "8.2s", .share = 0.14});
            s.blank();
            s.heading("Rates");
            s.entry({.label = "Hit rate", .value = "80%", .share = 0.8});
            s.entry({.label = "Turns using cache", .value = "8", .share = 1.0});
            return s.build();
        };

        for (int w = 60; w <= 300; w += 10) {
            INFO("width=", w);
            CHECK(measure_element(make(natural + 10), w).height.value
                  == measure_element(single(), w).height.value);
        }
    }

    SUBCASE("a sheet that overflows splits, and gets shorter for it") {
        // Tight budget on a surface wide enough to hold two real columns.
        // The split has to actually BUY something -- a split that does not
        // shorten the sheet has charged the reader a sideways journey for
        // nothing, and decide() declines those.
        //
        // Asserted on the PAINTED layout, not on measure_element(). The
        // measure callback deliberately reports a CEILING -- the tallest
        // layout the sheet has, so a scrolling host can never under-budget
        // -- so it reads the same whether or not a split happened. Using
        // it here would be asking the wrong oracle: it is doing its job by
        // not varying.
        auto rows_painted = [](const Element& el, int w) {
            StylePool pool;
            Canvas canvas(w, 80, &pool);
            render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
            int last = -1;
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < w; ++x) {
                    const auto ch = canvas.get(x, y).character;
                    if (ch != 0 && ch != U' ') { last = y; break; }
                }
            return last + 1;
        };

        auto bulky = [](int budget, int max_cols) {
            StatSheet s;
            s.indent(1);
            s.reserve_right(7);
            s.columns(max_cols);
            if (budget) s.height_budget(budget);
            for (int section = 0; section < 4; ++section) {
                if (section) s.blank();
                s.heading("Section " + std::to_string(section));
                for (int row = 0; row < 5; ++row)
                    s.entry({.label = "metric " + std::to_string(row),
                             .value = "42",
                             .share = 0.5});
            }
            return s.build();
        };

        // Baseline: splitting forbidden outright. "No budget" would not
        // do, because no budget means the host has no opinion and the
        // sheet falls back to splitting on width.
        const int unsplit = rows_painted(bulky(0, 1), 200);

        // A budget a two-column layout can actually reach. Roughly half
        // the unsplit height is what a balanced split produces, so ask for
        // a little more than that.
        const int split = rows_painted(bulky(unsplit / 2 + 2, 2), 200);
        INFO("unsplit=", unsplit, " split=", split);
        CHECK(split < unsplit);
    }

    SUBCASE("a split that cannot reach the budget is not taken") {
        // The cost/benefit rule. Splitting is not free -- it makes the
        // reader's eye travel sideways -- so it is only worth taking when
        // it actually achieves the FIT.
        //
        // "Any shortening is progress" is the tempting rule and it is
        // wrong: a sheet that goes from 33 rows to 29 against an 18-row
        // viewport still scrolls, and now scrolls a two-column layout.
        // That is strictly worse than one honest column, and it is what
        // put a Tools tab into two columns AND left a scrollbar on screen.
        auto rows_painted = [](const Element& el, int w) {
            StylePool pool;
            Canvas canvas(w, 120, &pool);
            render_tree(el, canvas, pool, theme::dark, /*auto_height=*/true);
            int last = -1;
            for (int y = 0; y < 120; ++y)
                for (int x = 0; x < w; ++x) {
                    const auto ch = canvas.get(x, y).character;
                    if (ch != 0 && ch != U' ') { last = y; break; }
                }
            return last + 1;
        };

        auto tall = [](int budget, int max_cols) {
            StatSheet s;
            s.indent(1);
            s.reserve_right(7);
            s.columns(max_cols);
            if (budget) s.height_budget(budget);
            for (int section = 0; section < 6; ++section) {
                if (section) s.blank();
                s.heading("Section " + std::to_string(section));
                for (int row = 0; row < 6; ++row)
                    s.entry({.label = "metric " + std::to_string(row),
                             .value = "42",
                             .share = 0.5});
            }
            return s.build();
        };

        // The true single-column height, from a sheet that is not allowed
        // to split at all. Using "no budget" as the baseline would not
        // work: no budget means the host has no opinion, and the sheet
        // then splits on width like it always did.
        const int unsplit = rows_painted(tall(0, 1), 200);

        // A budget so small that no two-column layout can reach it. The
        // sheet must decline to split rather than take a partial win.
        const int hopeless = rows_painted(tall(3, 2), 200);
        INFO("unsplit=", unsplit, " with-hopeless-budget=", hopeless);
        CHECK(hopeless == unsplit);

        // And a budget a split CAN reach is still taken.
        const int reachable = rows_painted(tall(unsplit / 2 + 2, 2), 200);
        INFO("unsplit=", unsplit, " reachable=", reachable);
        CHECK(reachable < unsplit);
    }

    SUBCASE("more width never costs a column's worth of content") {
        // Monotonicity, for the right reason: width is no longer what
        // decides, so a wider surface can only ever need FEWER columns.
        int prev = 1 << 30;
        for (int w = 80; w <= 300; w += 10) {
            const int h = measure_element(make(4), w).height.value;
            INFO("width=", w, " height=", h, " prev=", prev);
            CHECK(h <= prev);
            prev = h;
        }
    }
}

TEST_CASE("stat sheet: a sheet that cannot split still reports its full height") {
    // The degenerate case the fix has to keep working: one section, so no
    // split is possible at any width and the reported height must simply
    // be the height it paints.
    StatSheet s;
    s.indent(1);
    s.columns(2);
    s.heading("By model");
    s.entry({.label = "claude-sonnet-4-5", .value = "4", .share = 0.5});
    s.entry({.label = "claude-opus-4-5", .value = "2", .share = 0.25});
    s.entry({.label = "claude-haiku-4-5", .value = "2", .share = 0.25});
    const auto el = s.build();

    const int reported = measure_element(el, 1 << 14).height.value;
    for (int w = 40; w <= 320; w += 4) {
        INFO("width=", w);
        CHECK(measure_element(el, w).height.value <= reported);
    }
}
