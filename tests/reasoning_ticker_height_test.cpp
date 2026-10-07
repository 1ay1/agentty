// reasoning_ticker_height_test — the live reasoning block never shrinks.
//
// agentty renders live reasoning as a "thought ticker" when Appearance ▸
// Thinking is Collapsed (the default): ReasoningStream::Config::
// live_tail_rows = 8, so a long chain-of-thought stays a glance instead of
// shoving the composer down.
//
// The window is counted in ROWS, and that is the whole point. A window of
// line-NODES cannot hold a height: a node wraps to a variable number of
// rows, so the block dipped whenever a shorter paragraph became the newest
// node (8 → 13 → 12 → 9 at width 50). Every dip is a shrink mid-stream,
// which yanks the composer up under the user's cursor and, once rows have
// passed the viewport top, rewrites immutable scrollback.
//
// Padding up to a running row max was tried and is worse to look at:
// permanent blank rows under the newest line. The row window needs neither
// — the engine lays the body out at its natural height and the viewport
// scrolls to the bottom, so the height is exactly min(content, cap): it
// grows to the cap, then holds, with every row carrying real text.
//
// What this pins: monotonic height across a byte-by-byte stream, the cap
// itself, no dead rows, and the settled block releasing the window.

#include "agtest.hpp"

#include <maya/core/anim_clock.hpp>
#include <maya/print.hpp>
#include <maya/widget/reasoning.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 50;
constexpr int kCap   = 8;     // agentty's Collapsed default (turn.cpp)

int rows_of(const std::string& s) {
    int n = 1;
    for (const char c : s) if (c == '\n') ++n;
    return n;
}

maya::ReasoningStream::Config ticker_cfg() {
    maya::ReasoningStream::Config cfg;
    cfg.live_tail_rows = kCap;
    return cfg;
}

// Render a live ticker over `src` with the reveal allowed to catch up, and
// return the frame.
std::string ticker_frame(const std::string& src) {
    maya::ReasoningStream rs{ticker_cfg()};
    rs.set_live(true);
    rs.set_content(src);
    // The body is a reveal widget: let the typewriter reach the edge so the
    // measurement is of settled-shape content, not of a half-typed frame.
    std::string out;
    for (int f = 0; f < 400; ++f) {
        out = maya::render_to_string(rs.build(), kWidth);
        maya::testing::advance_anim_clock_ms(33);
    }
    return out;
}

int ticker_rows(const std::string& src) { return rows_of(ticker_frame(src)); }

// A chain of thought with the shapes that moved the height around: long
// paragraphs that wrap, short ones between them, a list, and a short final
// line (the common worst case — "the last para makes the block short").
const std::vector<std::string>& paragraphs() {
    static const std::vector<std::string> p = {
        "This is a long first paragraph that will certainly wrap across "
        "several terminal rows because it keeps going and going with plenty "
        "of words in it.",
        "Here is a second long paragraph, also comfortably long enough to "
        "wrap onto multiple rows when rendered at a narrow width like this.",
        "Short one.",
        "A fourth long paragraph that likewise wraps over several rows, so "
        "the window is full of tall nodes while these are the newest.",
        "- a list item\n- another list item",
        "A sixth paragraph of roughly the same size as the others, which is "
        "what a real chain of thought looks like after a while of thinking.",
        "Short final.",
    };
    return p;
}

}  // namespace

TEST_CASE("reasoning ticker: height never shrinks as paragraphs arrive") {
    maya::testing::freeze_anim_clock(0);

    std::string src;
    std::vector<int> heights;
    for (const auto& p : paragraphs()) {
        src += p;
        src += "\n\n";
        heights.push_back(ticker_rows(src));
    }

    maya::testing::unfreeze_anim_clock();

    std::string trace;
    for (std::size_t i = 0; i < heights.size(); ++i)
        trace += " p" + std::to_string(i + 1) + "=" + std::to_string(heights[i]);
    INFO("ticker heights:", trace);

    for (std::size_t i = 1; i < heights.size(); ++i)
        CHECK(heights[i] >= heights[i - 1]);

    // And it settles ON the cap rather than drifting above it. The chrome
    // (header + the block's own padding) rides along, so allow for it, but
    // the body itself must be exactly the window.
    CHECK(heights.back() <= kCap + 4);
}

TEST_CASE("reasoning ticker: height never shrinks byte by byte") {
    // The stricter version of the same invariant: every intermediate
    // prefix, not just paragraph boundaries. This is the shape a provider
    // actually delivers.
    maya::testing::freeze_anim_clock(0);

    std::string all;
    for (const auto& p : paragraphs()) { all += p; all += "\n\n"; }

    int prev = 0;
    int shrinks = 0;
    int worst_at = 0;
    maya::ReasoningStream rs{ticker_cfg()};
    rs.set_live(true);
    for (std::size_t n = 1; n <= all.size(); n += 7) {
        const std::string src = all.substr(0, n);
        rs.set_content(src);
        const int rows = rows_of(maya::render_to_string(rs.build(), kWidth));
        maya::testing::advance_anim_clock_ms(33);
        if (rows < prev) { ++shrinks; if (!worst_at) worst_at = static_cast<int>(n); }
        prev = rows;
    }

    maya::testing::unfreeze_anim_clock();

    INFO("shrinks:", shrinks, " first at byte ", worst_at, " of ", all.size());
    CHECK(shrinks == 0);
}

TEST_CASE("reasoning ticker: the window spends its rows on text") {
    maya::testing::freeze_anim_clock(0);

    std::string src;
    for (const auto& p : paragraphs()) { src += p; src += "\n\n"; }
    const std::string frame = ticker_frame(src);

    maya::testing::unfreeze_anim_clock();

    std::vector<std::string> lines;
    std::string cur;
    for (const char c : frame) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);

    const auto blank = [](const std::string& l) {
        return l.find_first_not_of(" \t\u2502\u2503") == std::string::npos;
    };

    // The window is cut at row granularity, so it never reserves rows it
    // cannot fill: no stack of blank rows anywhere (that was the padding
    // fix's failure mode), and at most the chrome's own couple of rows
    // below the last line of text.
    int run = 0, worst_run = 0, trailing = 0;
    for (const auto& l : lines) {
        if (blank(l)) { ++run; ++trailing; worst_run = std::max(worst_run, run); }
        else { run = 0; trailing = 0; }
    }
    INFO("frame:\n", frame);
    CHECK(worst_run <= 2);
    CHECK(trailing <= 2);

    // The newest text is what survives the window — the ticker is anchored
    // at the bottom, where the reveal caret is.
    CHECK(frame.find("Short final.") != std::string::npos);
    // ...and the oldest text has scrolled out of it.
    CHECK(frame.find("This is a long first paragraph") == std::string::npos);
}

TEST_CASE("reasoning ticker: one long paragraph is cut mid-wrap") {
    // The cut has to land INSIDE a node, not just between nodes: a single
    // paragraph longer than the window is the case a node-counted window
    // could not window at all (one node = all or nothing).
    maya::testing::freeze_anim_clock(0);

    const std::string one =
        "The first thing to establish is what the window actually promises, "
        "because the promise is what makes it safe to put above a composer. "
        "It promises a height: once the body is taller than the cap, the "
        "block is exactly the cap and stays there, no matter which words "
        "happen to be newest or how they wrap. The second thing is what it "
        "shows, which is simply the newest rows, so the reveal caret is "
        "always on screen at the bottom edge where the text is arriving.\n\n";

    const std::string frame = ticker_frame(one);
    maya::testing::unfreeze_anim_clock();

    INFO("frame:\n", frame);
    // The tail is on screen, the head has scrolled out, and the height is
    // the cap plus the block's own chrome.
    CHECK(frame.find("arriving") != std::string::npos);
    CHECK(frame.find("The first thing to establish") == std::string::npos);
    CHECK(rows_of(frame) <= kCap + 4);
    CHECK(rows_of(frame) >= kCap);
}

TEST_CASE("reasoning ticker: a delta ending on a paragraph break wastes no rows") {
    // How reasoning actually arrives: every burst ends with "\n\n". The
    // trailing gap under the newest line used to eat two of the window's
    // rows — cropping the oldest row to pay for a row with nothing in it —
    // and during a pause in the stream it sat there as dead space. The
    // answer lane never had this (StreamingMarkdown drops its trailing
    // empties); the reasoning window has to match it.
    maya::testing::freeze_anim_clock(0);

    std::string src;
    for (const auto& p : paragraphs()) { src += p; src += "\n\n"; }

    const std::string mid   = ticker_frame(src + "and then the wire stopped");
    const std::string brk   = ticker_frame(src + "and then the wire stopped.\n\n");

    maya::testing::unfreeze_anim_clock();

    const auto trailing_blanks = [](const std::string& frame) {
        std::vector<std::string> lines;
        std::string cur;
        for (const char c : frame) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        int n = 0;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->find_first_not_of(" \t\u2502\u2503") == std::string::npos) ++n;
            else break;
        }
        return n;
    };

    INFO("mid-sentence frame:\n", mid, "\nparagraph-break frame:\n", brk);
    CHECK(trailing_blanks(mid) == 0);
    CHECK(trailing_blanks(brk) == 0);
    // Same height either way: the break costs nothing.
    CHECK(rows_of(brk) == rows_of(mid));
    // ...and the newest line is still the last thing in the block.
    CHECK(brk.find("the wire stopped.") != std::string::npos);
}

TEST_CASE("reasoning: with no window at all, the block only grows") {
    // Thinking::Shown — the whole block, no cap. Monotonic for the other
    // reason: nothing is ever dropped, so the only question is whether the
    // markdown underneath re-flows shorter mid-stream. It must not.
    maya::testing::freeze_anim_clock(0);

    std::string all;
    for (const auto& p : paragraphs()) { all += p; all += "\n\n"; }

    maya::ReasoningStream::Config cfg;   // live_tail_rows stays 0
    maya::ReasoningStream rs{cfg};
    rs.set_live(true);

    int prev = 0, shrinks = 0, worst_at = 0;
    for (std::size_t n = 1; n <= all.size(); n += 7) {
        rs.set_content(all.substr(0, n));
        const int rows = rows_of(maya::render_to_string(rs.build(), kWidth));
        maya::testing::advance_anim_clock_ms(33);
        if (rows < prev) { ++shrinks; if (!worst_at) worst_at = static_cast<int>(n); }
        prev = rows;
    }

    maya::testing::unfreeze_anim_clock();

    INFO("shrinks:", shrinks, " first at byte ", worst_at, " of ", all.size());
    CHECK(shrinks == 0);
    // ...and it really is the whole block, not a window.
    CHECK(prev > kCap * 2);
}

TEST_CASE("reasoning: the window is the host's call, not the live flag") {
    // The cap used to be gated on set_live(), so a block settling released
    // the window and unfolded the whole chain-of-thought. That tied two
    // unrelated questions together: "is the model thinking right now" (the
    // header) and "how much reasoning does the reader want" (the window).
    // agentty answers the second with the Thinking pref, so Collapsed keeps
    // the tail after settling and Shown passes no cap at all.
    maya::testing::freeze_anim_clock(0);

    std::string src;
    for (const auto& p : paragraphs()) { src += p; src += "\n\n"; }

    const int live_rows = ticker_rows(src);

    // Same cap, settled: still a glance, same height.
    maya::ReasoningStream rs{ticker_cfg()};
    rs.set_content(src);
    rs.set_live(false);
    rs.finish();
    const std::string settled = maya::render_to_string(rs.build(), kWidth);

    // No cap (Thinking::Shown): the whole thing.
    maya::ReasoningStream::Config full_cfg;   // live_tail_rows stays 0
    maya::ReasoningStream full{full_cfg};
    full.set_content(src);
    full.set_live(false);
    full.finish();
    const std::string all = maya::render_to_string(full.build(), kWidth);

    maya::testing::unfreeze_anim_clock();

    INFO("live:", live_rows, " settled:", rows_of(settled),
         " uncapped:", rows_of(all));
    CHECK(rows_of(settled) == live_rows);
    CHECK(settled.find("This is a long first paragraph") == std::string::npos);
    CHECK(rows_of(all) > live_rows);
    CHECK(all.find("This is a long first paragraph") != std::string::npos);
}
