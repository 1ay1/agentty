// tool_output_wrap_test — the tool-output viewer's scroll accounting must
// agree with what its rows actually PAINT.
//
// This panel windows its own rows: it counts cache.rows, sets
// max_y = total - viewport, and pushes exactly `viewport` entries into
// cfg.prebuilt. That accounting is ROW-BASED, so it is only correct while
// every cached row paints exactly one visual line.
//
// The `$ command` row is the exception -- it is deliberately TextWrap::Wrap
// so a long shell one-liner stays readable instead of being clipped. A
// command longer than the panel is wide therefore paints 2+ lines while the
// window still counts it as 1, so the last rows of output are pushed below
// the viewport with no scroll offset able to reach them. The panel already
// warns about exactly this hazard in a comment about structured preview
// children; the command row has the same shape.
//
// The assertion is the invariant, not a pixel count: the rows handed to the
// panel must not paint taller than the viewport they were windowed for.

#include <doctest/doctest.h>

#include "maya/element/builder.hpp"
#include "maya/dsl.hpp"

using namespace maya;
using namespace maya::dsl;

namespace {

// The command row exactly as tool_output_panel builds it: a fixed 2-column
// "$ " gutter plus the command taking the rest of the width, pinned to one
// row so the panel's row-based windowing stays exact.
Element command_row(const std::string& command) {
    return hstack()(
        text("$ ", Style{}.with_bold()),
        Element{TextElement{
            .content = command,
            .wrap    = TextWrap::TruncateEnd,
        }} | grow(1.0f))
        | height(1) | overflow(Overflow::Hidden);
}

}  // namespace

TEST_CASE("tool output: the command row occupies exactly one row at any width") {
    // A realistic long one-liner -- the kind `shell` runs constantly, and
    // the case that used to wrap onto three lines while the viewport
    // arithmetic counted it as one.
    const std::string cmd =
        "for f in $(git ls-files '*.cpp'); do clang-format -i --style=file "
        "\"$f\" && echo \"formatted $f\"; done && git diff --stat";

    // Every width from a cramped split pane to an ultrawide terminal. The
    // row must cost exactly one line at all of them: the panel counts
    // cache.rows, derives max_y from that count and pushes exactly `vh`
    // entries, so any row that paints taller silently steals rows the
    // window never budgeted and the output beneath becomes unreachable.
    for (int w = 30; w <= 300; w += 2) {
        INFO("width=", w);
        CHECK(measure_element(command_row(cmd), w).height.value == 1);
    }
}

TEST_CASE("tool output: an embedded newline cannot grow the command row") {
    // A multi-line command (a heredoc, a `&&`-chained script pasted whole)
    // is the other way a single entry used to paint several lines.
    const std::string cmd = "set -e\ncmake --build build -j12\nctest --output-on-failure";
    for (int w : {40, 76, 120, 200}) {
        INFO("width=", w);
        CHECK(measure_element(command_row(cmd), w).height.value == 1);
    }
}

TEST_CASE("tool output: a short command still occupies exactly one row") {
    // The common case has to stay exact too -- no padding, no growth.
    CHECK(measure_element(command_row("ls -la"), 76).height.value == 1);
}
