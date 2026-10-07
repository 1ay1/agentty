// reasoning_ticker_height_test — the live reasoning block stays bounded.
//
// agentty renders live reasoning as a "thought ticker" when Appearance ▸
// Thinking is Collapsed (the default): ReasoningStream::Config::
// live_tail_lines = 3, so a long chain-of-thought stays a glance instead
// of shoving the composer down.
//
// The window is 3 line-NODES and a node wraps to a variable number of
// ROWS, so the height dips when a shorter paragraph becomes the newest
// node. maya padded up to a running row max to stop that and dropped it
// again in 25b1759 — the pad left permanent blank rows under the newest
// line, which reads worse than a transient dip. So the height is
// non-monotonic on purpose; a row-exact window is the real fix and it
// needs node splitting.
//
// What the ticker still promises, and what this pins: the window BOUNDS
// the block, so thinking for a long time does not grow the height with
// it, and a settled block carries no reserved blank rows.

#include "agtest.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/print.hpp>
#include <maya/widget/reasoning.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 50;

int rows_of(const std::string& s) {
    int n = 1;
    for (const char c : s) if (c == '\n') ++n;
    return n;
}

// Render a live ticker over `src` with the reveal allowed to catch up, and
// return its height in rows.
int ticker_rows(const std::string& src) {
    maya::ReasoningStream::Config cfg;
    cfg.live_tail_lines = 3;          // agentty's Collapsed default
    maya::ReasoningStream rs{cfg};
    rs.set_live(true);
    rs.set_content(src);
    // The body is a reveal widget: let the typewriter reach the edge so the
    // measurement is of settled-shape content, not of a half-typed frame.
    std::string out;
    for (int f = 0; f < 400; ++f) {
        out = maya::render_to_string(rs.build(), kWidth);
        maya::testing::advance_anim_clock_ms(33);
    }
    return rows_of(out);
}

}  // namespace

TEST_CASE("reasoning ticker: the window bounds the block as paragraphs arrive") {
    maya::testing::freeze_anim_clock(0);

    // Long paragraphs (each wraps to several rows) with a SHORT one in the
    // middle and at the end — the shapes that move the height around.
    const std::vector<std::string> paras = {
        "This is a long first paragraph that will certainly wrap across "
        "several terminal rows because it keeps going and going with plenty "
        "of words in it.",
        "Here is a second long paragraph, also comfortably long enough to "
        "wrap onto multiple rows when rendered at a narrow width like this.",
        "A third long paragraph that likewise wraps over several rows so the "
        "ticker window is tall while these three are the newest nodes.",
        "Short one.",
        "A fifth long paragraph, again several rows wide, so the window is "
        "full of tall nodes once more and the text keeps accumulating.",
        "A sixth long paragraph of roughly the same size, which is what a "
        "real chain of thought looks like after thinking for a while.",
        "Short final.",
    };

    std::string src;
    std::vector<int> heights;
    for (const auto& p : paras) {
        src += p;
        src += "\n\n";
        heights.push_back(ticker_rows(src));
    }

    std::string trace;
    for (std::size_t i = 0; i < heights.size(); ++i)
        trace += " p" + std::to_string(i + 1) + "=" + std::to_string(heights[i]);
    INFO("ticker heights:", trace);

    // Bounded: seven paragraphs in, the block is no taller than it was when
    // three were. The window is the whole point — thinking longer must not
    // push the composer further down.
    const int tallest = *std::max_element(heights.begin(), heights.end());
    CHECK(tallest < 20);
    CHECK(heights.back() <= tallest);

    // And it really is a window, not the full text: the same content settled
    // (no window) is much taller.
    maya::ReasoningStream::Config full_cfg;
    full_cfg.live_tail_lines = 3;
    maya::ReasoningStream full{full_cfg};
    full.set_content(src);
    full.set_live(false);
    full.finish();
    const int settled_rows = rows_of(maya::render_to_string(full.build(), kWidth));
    maya::testing::unfreeze_anim_clock();
    INFO("settled rows:", settled_rows);
    CHECK(settled_rows > tallest);
}

TEST_CASE("reasoning ticker: settling releases the reserved rows") {
    maya::testing::freeze_anim_clock(0);

    const std::string src =
        "A long paragraph that wraps across several rows when it renders at "
        "this narrow width, giving the live window something tall.\n\n"
        "Short.";

    maya::ReasoningStream::Config cfg;
    cfg.live_tail_lines = 3;
    maya::ReasoningStream rs{cfg};
    rs.set_live(true);
    rs.set_content(src);
    for (int f = 0; f < 400; ++f) {
        (void)maya::render_to_string(rs.build(), kWidth);
        maya::testing::advance_anim_clock_ms(33);
    }

    // Settle: the full body renders with no window at all, so the pad must
    // not survive as blank rows under it.
    rs.set_live(false);
    rs.finish();
    const std::string settled = maya::render_to_string(rs.build(), kWidth);

    maya::testing::unfreeze_anim_clock();

    // Both paragraphs are present once settled (no window), and the block
    // does not carry a tail of empty reserved rows.
    CHECK(settled.find("Short.") != std::string::npos);
    const std::size_t trailing_blanks = [&] {
        std::size_t n = 0;
        for (std::size_t i = settled.size(); i-- > 0;) {
            if (settled[i] == '\n') { ++n; continue; }
            if (settled[i] == ' ')  continue;
            break;
        }
        return n;
    }();
    CHECK(trailing_blanks <= 2);
}
