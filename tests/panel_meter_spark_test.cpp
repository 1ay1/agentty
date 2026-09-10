// panel_meter_spark_test — the two read-only drawn controls.
//
// Meter and Spark are separate kinds because their WIDTH RULES are
// opposites, and that is the property most worth pinning:
//
//   Meter fills its budget. More columns draw a finer scale of the same
//   fact, so a wider panel means a more precise reading.
//
//   Spark is one cell per SAMPLE. Its natural width is the data; a budget
//   is a window onto the tail, never a size to stretch to. Padding a spark
//   to a meter's width pads with blank -- and that blank is what once
//   pushed a value column off the end of its row when the two shared a
//   single width.
//
// So: "does it get wider" is the right question for one and the wrong
// question for the other, and a test that asked it of both would be
// enforcing the bug.

#include "agtest.hpp"

#include <maya/widget/panel/context.hpp>
#include <maya/widget/panel/item/meter.hpp>
#include <maya/widget/panel/item/spark.hpp>
#include <maya/widget/panel/theme.hpp>

#include <string>
#include <vector>

using maya::StyledRun;
using maya::panel::ItemCtx;
using maya::panel::Meter;
using maya::panel::Spark;

namespace {

const maya::panel::Theme& theme() {
    static const maya::panel::Theme t{};
    return t;
}

// Cells in a rendered string: every glyph these kinds emit is 3 bytes in
// UTF-8 (block elements and box-drawing all live in U+2500..U+259F).
int cells_of(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); i += 3) ++n;
    return n;
}

std::string meter_at(double share, int budget) {
    ItemCtx ctx{.theme = theme(), .draw_budget = budget};
    return render(Meter{.share = share}, ctx).first;
}

std::string spark_at(const std::vector<double>& xs, int budget) {
    ItemCtx ctx{.theme = theme(), .draw_budget = budget};
    return render(Spark{.series = xs}, ctx).first;
}

}  // namespace

TEST_CASE("meter: fills its budget") {
    // The property that makes it a meter. A frozen-width bar was the
    // original defect in the stats sheet's track.
    CHECK(cells_of(meter_at(0.5, 10)) < cells_of(meter_at(0.5, 20)));
    CHECK(cells_of(meter_at(0.5, 20)) < cells_of(meter_at(0.5, 32)));
}

TEST_CASE("meter: bounded at both ends") {
    // A floor, because below it a bar stops reading as a scale.
    CHECK(cells_of(meter_at(0.5, 1)) >= 8);
    // A ceiling, because past it the eye measures cells instead of
    // comparing lengths -- and the number beside it is the better answer.
    CHECK(cells_of(meter_at(0.5, 400)) <= 40);
}

TEST_CASE("meter: the track is drawn, not left blank") {
    // An empty tail makes a LOW bar read as a MISSING bar -- the row looks
    // broken rather than small. The visible track is what says "this is
    // the scale, and you are here on it".
    const std::string low = meter_at(0.1, 24);
    CHECK(low.find("\xe2\x94\x80") != std::string::npos);   // ─ remainder
    CHECK(low.find("\xe2\x96\x88") != std::string::npos);   // █ fill
    // Fill + track together are the whole width: no blanks in between.
    CHECK(cells_of(low) == 24);
}

TEST_CASE("meter: the share is clamped, not trusted") {
    // Live telemetry can produce a share a hair over 1 on a rounding edge,
    // and a bar that overruns its own track reads as a rendering fault.
    CHECK(cells_of(meter_at(1.5, 24)) == 24);
    CHECK(cells_of(meter_at(-0.5, 24)) == 24);
    // Full means full: every cell is fill, none is track.
    CHECK(meter_at(1.5, 24).find("\xe2\x94\x80") == std::string::npos);
}

TEST_CASE("meter: two hues, because the boundary is the datum") {
    // A bar painted in one colour is a rectangle. The filled/unfilled
    // boundary is the entire reading, so the control must be able to say
    // where it is.
    std::vector<StyledRun> runs;
    ItemCtx ctx{.theme = theme(), .draw_budget = 24, .runs_out = &runs};
    const auto [text, _] = render(Meter{.share = 0.5}, ctx);
    CHECK(runs.size() == 2);
    if (runs.size() == 2) {
        // Contiguous and complete: no byte unstyled, none styled twice.
        CHECK(runs[0].byte_offset == 0);
        CHECK(runs[0].byte_offset + runs[0].byte_length == runs[1].byte_offset);
        CHECK(runs[1].byte_offset + runs[1].byte_length == text.size());
    }
}

TEST_CASE("spark: one cell per sample, NOT stretched to the budget") {
    // The rule that makes it a different kind. Four samples are four cells
    // on any surface -- stretching them would invent data, and padding
    // them with blank is what pushed a value column off its row.
    const std::vector<double> four{1, 2, 3, 4};
    CHECK(cells_of(spark_at(four, 8))  == 4);
    CHECK(cells_of(spark_at(four, 24)) == 4);
    CHECK(cells_of(spark_at(four, 40)) == 4);
}

TEST_CASE("spark: the budget is a window on the tail") {
    // More samples than room: show the most RECENT ones. A trend strip
    // showing the oldest window answers a question nobody asked.
    std::vector<double> many;
    for (int i = 0; i < 100; ++i) many.push_back(i);
    const std::string s = spark_at(many, 12);
    CHECK(cells_of(s) == 12);
    // The tail of 0..99 is the high end, so the last cell is the peak.
    CHECK(s.substr(s.size() - 3) == "\xe2\x96\x88");   // █
}

TEST_CASE("spark: a flat series draws at the floor, not the ceiling") {
    // All-equal samples scaled against their own max would draw a solid
    // wall of █ and claim a maximum they never reached. The low strip says
    // "nothing is happening", which is the truth.
    const std::vector<double> flat{5, 5, 5, 5};
    const std::string s = spark_at(flat, 24);
    CHECK(s.find("\xe2\x96\x88") == std::string::npos);   // no █
    CHECK(cells_of(s) == 4);
}

TEST_CASE("spark: an empty series draws nothing") {
    // Not a zero-height strip, not a row of floors: nothing. A series with
    // no samples has no shape to show, and drawing one would be inventing
    // a reading.
    CHECK(spark_at({}, 24).empty());
}

TEST_CASE("meter and spark: an unmeasured budget still draws") {
    // Built outside a panel (a preview, a test, a host that has not
    // measured yet), budget is 0. Both must fall back to a preference --
    // a control that vanishes when unmeasured vanishes in exactly the case
    // nobody tests.
    CHECK(cells_of(meter_at(0.5, 0)) == 24);
    CHECK(cells_of(spark_at({1, 2, 3}, 0)) == 3);
}
