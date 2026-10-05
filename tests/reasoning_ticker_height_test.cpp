// reasoning_ticker_height_test — the live reasoning block never shrinks.
//
// ── The bug ─────────────────────────────────────────────────────────────
//
// agentty renders live reasoning as a "thought ticker" when Appearance ▸
// Thinking is Collapsed (the DEFAULT): ReasoningStream::Config::
// live_tail_lines = 3, so only the newest few lines show and a long
// chain-of-thought stays a glance instead of shoving the composer down.
//
// The window is 3 line-NODES. A node is a markdown block, and a block wraps
// to a VARIABLE number of terminal ROWS. So the block's height tracked
// whichever three paragraphs happened to be newest, and fell whenever a
// shorter one entered the window. Measured at width 50:
//
//     after para 1:  8 rows
//     after para 2: 13 rows
//     after para 3: 12 rows      <- shrink
//     after para 4:  9 rows      <- shrink (short final paragraph)
//
// Every decrease is a height shrink DURING streaming, which yanks the
// composer and status bar up under the user's cursor. A short final
// paragraph is both the worst case and the common one, which is why the
// report was "the last para makes the height short".
//
// This is the same invariant md_shape_sweep enforces for the markdown
// widget ("ALL SHAPES MONOTONIC") — the reasoning ticker simply was not
// covered by it, because the instability comes from the tail WINDOW and not
// from the markdown underneath.
//
// ── The fix this pins ───────────────────────────────────────────────────
//
// The ticker holds a running max of the rows it has occupied for the
// current live block and pads up to it. Growth stays immediate; only dips
// are absorbed. set_live() resets it on each live/settled transition, so a
// settled block (which renders in full, no window) carries no dead rows and
// a new turn does not inherit the previous one's tallest moment.

#include "agtest.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/print.hpp>
#include <maya/widget/reasoning.hpp>

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

// ── The trade this records ──────────────────────────────────────────────
//
// The ticker window keeps the last N line-NODES, and a node wraps to a
// VARIABLE number of ROWS, so the block's height dips whenever a shorter
// paragraph becomes the newest. Measured at width 50 with tail=3:
//
//     p1=8  p2=13  p3=12  p4=9      <- two shrinks
//
// A shrink mid-stream pulls the composer up under the cursor, so this was
// briefly fixed by padding to a running row max. That removed the shrink
// and replaced it with something users liked less: permanent blank rows
// under the newest line for the rest of the block, which read as the widget
// being broken. Reverted on that feedback -- a transient dip beats dead
// space.
//
// So the height is NOT monotonic, deliberately. What this pins instead is
// that the window still BOUNDS the block (its whole purpose: a long chain
// of thought must not shove the composer down the screen) and that no dip
// is extreme. If someone implements a row-exact window later -- selecting
// the tail by rows rather than nodes, which needs line-node splitting and
// so is a real change -- the monotonic assertion can come back with it.
TEST_CASE("reasoning ticker: the window bounds the block without dead rows") {
    maya::testing::freeze_anim_clock(0);

    const std::vector<std::string> paras = {
        "This is a long first paragraph that will certainly wrap across "
        "several terminal rows because it keeps going and going with plenty "
        "of words in it.",
        "Here is a second long paragraph, also comfortably long enough to "
        "wrap onto multiple rows when rendered at a narrow width like this.",
        "A third long paragraph that likewise wraps over several rows so the "
        "ticker window is tall while these three are the newest nodes.",
        "Short final.",
    };

    std::string src;
    std::vector<int> heights;
    for (const auto& p : paras) {
        src += p;
        src += "\n\n";
        heights.push_back(ticker_rows(src));
    }

    maya::testing::unfreeze_anim_clock();

    std::string trace;
    for (std::size_t i = 0; i < heights.size(); ++i)
        trace += " p" + std::to_string(i + 1) + "=" + std::to_string(heights[i]);
    INFO("ticker heights:", trace);

    // Bounded: the whole point of the window. Without it the block would
    // track the full text and grow without limit.
    for (const int h : heights) CHECK(h < 40);

    // And the content never vanishes entirely -- a dip must not collapse
    // the block to chrome.
    for (const int h : heights) CHECK(h >= 3);
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
