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
