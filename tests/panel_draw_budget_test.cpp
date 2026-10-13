// panel_draw_budget_test — a drawn control sizes to its panel.
//
// THE GAP THIS CLOSES
// ===================
// maya::panel's item widgets render from their own value plus ItemCtx, and
// nothing else. That is the modularity guarantee, and it is a good one --
// an item widget physically cannot grow panel logic.
//
// But ItemCtx carried only facts a TEXT control needs. A control whose
// glyphs are a picture -- a bar, a meter, a strip -- had no way to learn how
// much room it was in, so every one hardcoded its own size. Slider drew
// `constexpr int kCells = 12` on a 40-column split pane and on a 200-column
// terminal alike.
//
// For a bar that is not a cosmetic issue: a bar's resolution IS its width.
// At 12 cells the smallest visible step is 8%, so two settings a twentieth
// apart draw identically. Freezing the size froze the information.
//
// ItemCtx::draw_budget is the missing fact, derived from min_width exactly
// as edit_budget is -- and for the reason edit_budget's comment gives:
// asking the LAYOUT how wide a cell is, inside a scroll viewport that
// measures children against an unbounded width, is what produced the
// "control reports 2^24 columns" class of bug.
//
// WHAT IS PINNED
// ==============
// Both directions, because a budget that always grows is as wrong as one
// that never does:
//   • a wider panel draws a wider bar        (the bug)
//   • bounded at both ends                   (not unbounded growth)
//   • unmeasured budget still draws          (used outside a panel)
//   • the ratio the bar encodes is unchanged (it is still the same datum)

#include "agtest.hpp"

#include <maya/widget/panel/context.hpp>
#include <maya/widget/panel/item/slider.hpp>
#include <maya/widget/panel/theme.hpp>

#include <maya/text/unicode_width.hpp>

#include <string>

using maya::panel::ItemCtx;
using maya::panel::Slider;

namespace {

const maya::panel::Theme& theme() {
    static const maya::panel::Theme t{};
    return t;
}

// Cells of bar in a rendered slider: the glyphs are ▆ (on) and ▁ (off), and
// the numeric value follows after two spaces.
int bar_cells(const Slider& s, int draw_budget) {
    ItemCtx ctx{.theme = theme(), .draw_budget = draw_budget};
    const auto [text, _] = render(s, ctx);
    int n = 0;
    for (std::size_t i = 0; i + 2 < text.size(); i += 3) {
        const std::string g = text.substr(i, 3);
        if (g == "\xe2\x96\x86" || g == "\xe2\x96\x81") ++n;
        else break;
    }
    return n;
}

}  // namespace

TEST_CASE("draw budget: a wider panel draws a wider bar") {
    const Slider s{.value = 0.5};
    // The reported defect: identical pictures at every width.
    CHECK(bar_cells(s, 10) < bar_cells(s, 20));
    CHECK(bar_cells(s, 20) < bar_cells(s, 24));
}

TEST_CASE("draw budget: the bar is bounded at both ends") {
    const Slider s{.value = 0.5};
    // A floor, because below it a bar stops being a scale and becomes
    // texture -- a narrow panel should draw a small bar, not a smear.
    CHECK(bar_cells(s, 1) >= 8);
    CHECK(bar_cells(s, 4) >= 8);
    // And a ceiling, because a bounded ratio is judged by proportion, not
    // measured. Unbounded growth turns a settings list into a bar chart.
    CHECK(bar_cells(s, 200) <= 24);
    CHECK(bar_cells(s, 4096) <= 24);
}

TEST_CASE("draw budget: an unmeasured budget still draws") {
    // A control built outside a panel (a preview, a test, a host that has
    // not measured yet) gets budget 0. It must fall back to its preference
    // rather than to zero -- a control that vanishes when unmeasured is a
    // control that vanishes in exactly the case nobody tests.
    const Slider s{.value = 0.5};
    CHECK(bar_cells(s, 0) == 12);
}

TEST_CASE("draw budget: resizing does not change the value the bar encodes") {
    // The bar got wider; it must still say the same thing. A half-full
    // slider is half-full at every width -- if the fill ratio drifted with
    // the budget, the picture would be lying about the datum.
    for (int budget : {0, 8, 12, 16, 20, 24, 64}) {
        const auto cells = bar_cells(Slider{.value = 0.5}, budget);
        ItemCtx ctx{.theme = theme(), .draw_budget = budget};
        const auto [text, _] = render(Slider{.value = 0.5}, ctx);
        int on = 0;
        for (std::size_t i = 0; i + 2 < text.size(); i += 3)
            if (text.substr(i, 3) == "\xe2\x96\x86") ++on; else break;
        INFO("budget=", budget, " cells=", cells, " on=", on);
        // Half, to within the rounding a discrete bar must do.
        CHECK(on * 2 >= cells - 1);
        CHECK(on * 2 <= cells + 1);
    }
}
