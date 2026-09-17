// native_visibility_test — nothing agentty paints goes invisible under
// theme::native.
//
// ── The bug ─────────────────────────────────────────────────────────────
//
// agentty #45: reasoning blocks rendered near-black on a black terminal —
// invisible, but selectable, so the text was demonstrably there. It was
// reported as a reasoning bug and was not one.
//
// A LitColor is PAINTABLE but not necessarily NUMERIC. Only Kind::Rgb has
// channels; Named and Indexed keep a PALETTE INDEX in the r_ byte with g_
// and b_ zero, and Default has nothing at all. maya's blend helpers read
// r()/g()/b() unconditionally, so a palette index became a red channel:
//
//     bright_black -> Named(8) -> lerp -> rgb(8,0,0) -> "38;2;8;0;0"
//
// theme::native states every slot as Named or Default ON PURPOSE — that is
// how the user's own palette, already contrast-checked by them, reaches the
// screen. Which makes native the one theme where every blend in the program
// is unsound, and the one theme no developer runs, because everybody picks
// a scheme.
//
// ── The invariant ───────────────────────────────────────────────────────
//
// Under native, NOTHING agentty renders may emit a truecolor SGR. Not
// "should mostly avoid" — a 38;2 sequence under native is by construction a
// colour the user did not choose, computed from bytes that were not
// channels. An effect that cannot be computed must degrade to NO EFFECT.
//
// maya/tests/test_blend_safety.cpp pins the helpers in isolation. This pins
// the property end-to-end, on real rendered agentty frames, which is the
// level #45 was actually reported at.
#include "agtest.hpp"

#include <string>
#include <vector>

#include <maya/core/animation.hpp>
#include <maya/core/anim_clock.hpp>   // advance_anim_clock_ms (reveal frames)
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>
#include <nlohmann/json.hpp>

#include "agentty/domain/ui_theme.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/palette.hpp"   // theme_owns_canvas

using namespace agentty;
using namespace maya;
using json = nlohmann::json;

namespace {

// Every slot of `native`, blended against every other at the parameters a
// fade actually visits. Returns the offending pairs.
std::vector<std::string> native_blend_offenders() {
    const Theme& th = theme::native;
    std::vector<std::string> bad;
    for (std::size_t i = 0; i < kThemeSlotCount; ++i) {
        const auto si = static_cast<ThemeSlot>(i);
        const LitColor a = th.resolve(Color::slot(si));
        for (std::size_t j = 0; j < kThemeSlotCount; ++j) {
            const auto sj = static_cast<ThemeSlot>(j);
            const LitColor b = th.resolve(Color::slot(sj));
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                if (anim::lerp(a, b, t).kind() == ColorKind::Rgb)
                    bad.push_back(std::string(slot_field_name(si)) + " x "
                                  + std::string(slot_field_name(sj)));
            }
        }
    }
    return bad;
}

} // namespace

TEST_CASE("native: every slot states a palette colour, never an RGB literal") {
    const Theme& th = theme::native;
    for (std::size_t i = 0; i < kThemeSlotCount; ++i) {
        const auto s = static_cast<ThemeSlot>(i);
        const LitColor c = th.resolve(Color::slot(s));
        const std::string name{slot_field_name(s)};
        const std::string lit_msg =
            "native slot '" + name + "' is an RGB literal — native exists to "
            "let the user's own palette through, so every slot must be Named "
            "or Default";
        CHECK(c.kind() != ColorKind::Rgb, lit_msg);
        const std::string ch_msg =
            "native slot '" + name + "' reports channels; arithmetic on it "
            "would read a palette index as colour";
        CHECK(!c.has_channels(), ch_msg);
    }
}

TEST_CASE("native: no pairwise slot blend fabricates a truecolor triple") {
    const auto bad = native_blend_offenders();
    const std::string msg =
        "native slot blend(s) fabricated an RGB triple — this is the agentty "
        "#45 shape (first: " + std::string(bad.empty() ? "-" : bad.front())
        + ")";
    CHECK(bad.empty(), msg);
}

TEST_CASE("native: bright_black stays SGR 90, never a truecolor triple") {
    // The exact reproduction from the issue, as a value. Muted is what the
    // reasoning body resolves to, and the settled path blends it with
    // itself on every line, so a flat blend MUST be the identity.
    const LitColor muted = theme::native.resolve(Color::slot(ThemeSlot::Muted));
    CHECK(muted.fg_sgr() == "90", "native muted must be bright-black (SGR 90)");
    for (double t : {0.0, 0.5, 1.0}) {
        const LitColor got = anim::lerp(muted, muted, t);
        const std::string msg =
            "a flat blend emitted '" + got.fg_sgr() + "' — expected the "
            "palette code 90, not arithmetic on the index";
        CHECK(got.fg_sgr() == "90", msg);
    }

    // Default (the `text` slot) must survive too: it has no channels at
    // all, and lerp used to turn it into literal black.
    const LitColor text = theme::native.resolve(Color::slot(ThemeSlot::Text));
    CHECK(text.fg_sgr() == "39", "native text must be the terminal default");
    CHECK(anim::lerp(text, text, 0.5).fg_sgr() == "39",
          "blending the default colour with itself must stay the default");
}

namespace {

// Render a model under native and return every truecolor cell found.
//
// Under native no slot has channels, so ANY Rgb foreground or background
// in the frame was computed from bytes that were not channels. Returning
// the offenders rather than a bool keeps the failure message able to name
// a row and column, which is what made the original diagnosis quick.
std::vector<std::string> truecolor_cells(Model& m, int w = 100, int h = 400) {
    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);

    std::vector<std::string> bad;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto& cell = canvas.get(x, y);
            const maya::Style& st = pool.get(cell.style_id);
            for (const auto* c : {&st.fg, &st.bg}) {
                if (!c->has_value()) continue;
                const LitColor lit = theme::native.resolve(**c);
                if (lit.kind() != ColorKind::Rgb) continue;
                bad.push_back("row " + std::to_string(y) + " col "
                              + std::to_string(x) + " -> " + lit.fg_sgr());
            }
        }
    }
    return bad;
}

// Every cell whose ink is the SAME COLOUR as the paper under it.
//
// This is the failure the truecolor sweep cannot see, and the one that
// actually shipped. Under native the 23 theme slots collapse onto the 16
// ANSI colours the terminal defines — so `success` and `diff_added` are
// both SGR 32, `text` and `inverse_text` are both SGR 39, and a widget
// that pairs two of them paints green-on-green or default-on-default.
//
// Both colours are perfectly legitimate palette entries. Nothing was
// fabricated, no channel was misread, the Themed gate is satisfied, and
// the text is invisible. Contrast is a property of the (slot-pair, theme)
// combination, and only the theme knows it — so it has to be checked on a
// rendered frame, not at the call site.
std::vector<std::string> invisible_cells(Model& m, int w = 100, int h = 400) {
    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);

    std::vector<std::string> bad;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto& cell = canvas.get(x, y);
            // Blank cells carry no ink, so same-colour is fine (and normal:
            // that is what a filled band's padding IS).
            if (cell.character == U' ' || cell.character == 0) continue;
            const maya::Style& st = pool.get(cell.style_id);
            if (!st.fg.has_value() || !st.bg.has_value()) continue;

            const LitColor fg = theme::native.resolve(*st.fg);
            const LitColor bg = theme::native.resolve(*st.bg);
            // Compare the BYTES that reach the terminal, not the slots.
            // Two different slot names can be one colour after resolution,
            // which is exactly the bug.
            if (fg.fg_sgr() != bg.fg_sgr()) continue;

            std::string glyph;
            if (cell.character < 128) glyph = std::string(1, char(cell.character));
            bad.push_back("row " + std::to_string(y) + " col "
                          + std::to_string(x) + " '" + glyph
                          + "' fg==bg==SGR " + fg.fg_sgr());
        }
    }
    return bad;
}

Message reasoning_msg(bool done) {
    Message a;
    a.role = Role::Assistant;
    a.id   = MessageId{"a1"};
    a.thinking = "Let me start by exploring the repository structure.\n\n"
                 "The user wants:\n\n"
                 "1. An audit of the **bwrap** implementation\n"
                 "2. A blueprint of what it would take\n\n"
                 "This is a blueprint task, no code editing.\n\n"
                 "The cwd is /home/jon/sandbox. Let me check the structure";
    if (done) a.text = "Here is the audit.";
    return a;
}

} // namespace

TEST_CASE("native: a rendered reasoning turn paints no truecolor cell") {
    // End-to-end, SETTLED: the state the first screenshot shows.
    ui_prefs::publish_theme(theme::native);

    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(reasoning_msg(/*done=*/true));
    m.s.phase = phase::Idle{};

    const auto bad = truecolor_cells(m);
    const std::string msg =
        "cell(s) painted a truecolor value under theme::native — native states "
        "no RGB, so every triple here was computed from bytes that were not "
        "channels (agentty #45). First: "
        + std::string(bad.empty() ? "-" : bad.front());
    CHECK(bad.empty(), msg);
}

TEST_CASE("native: a LIVE streaming reasoning block stays visible") {
    // The state the issue's screenshots ACTUALLY show: mid-stream, header
    // reading "Thinking", body still arriving. This path differs from the
    // settled one in the way that matters — ReasoningStream applies its
    // "stream of consciousness" GRADIENT while live, fading body_fg to
    // body_fg_bright down the block. That is a per-line lerp between two
    // theme slots, i.e. exactly the operation that produced rgb(8,0,0).
    //
    // The settled test alone would pass with the gradient still broken,
    // because a settled block renders flat. Reported-state coverage is not
    // optional here.
    ui_prefs::publish_theme(theme::native);

    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(reasoning_msg(/*done=*/false));
    m.s.phase = phase::Streaming{phase::Active{}};

    const auto bad = truecolor_cells(m);
    const std::string msg =
        "a LIVE reasoning block painted truecolor under native — the live "
        "gradient blends two slots per line, which is the #45 operation. "
        "First: " + std::string(bad.empty() ? "-" : bad.front());
    CHECK(bad.empty(), msg);
}

TEST_CASE("native: the reveal animation paints no truecolor") {
    // Reveal is ON by default everywhere, and it is the other per-cell
    // colour effect in the program: decorate_text_reveal ghosts and glides
    // freshly-arrived glyphs, and decorate_end_caret builds a backdrop by
    // dividing the caret colour's channels by four. Both are arithmetic on
    // a colour, both run on the live tail, and neither is exercised by a
    // settled render.
    ui_prefs::publish_theme(theme::native);
    ::setenv("AGENTTY_REVEAL", "1", 1);

    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(reasoning_msg(/*done=*/false));
    m.s.phase = phase::Streaming{phase::Active{}};

    // Several animation frames: the reveal window moves, so one frame is
    // not a sample of it.
    std::vector<std::string> bad;
    for (int frame = 0; frame < 8 && bad.empty(); ++frame) {
        maya::testing::advance_anim_clock_ms(16);
        bad = truecolor_cells(m);
    }
    ::unsetenv("AGENTTY_REVEAL");

    const std::string msg =
        "the reveal animation painted truecolor under native. First: "
        + std::string(bad.empty() ? "-" : bad.front());
    CHECK(bad.empty(), msg);
}

// ── The sweep ────────────────────────────────────────────────────
//
// The three cases above cover the reasoning widget, because that is what
// #45 named. Twice now the same bug has turned up in a DIFFERENT widget
// that nobody had rendered under native — the reveal animation, then the
// diff bands and the status banner. Each time the fix was local and each
// time the next instance was already sitting there.
//
// So stop chasing widgets and assert the property over the UI: put every
// major surface on screen at once and require the whole frame to be free
// of truecolor under native. A new widget that hardcodes a colour fails
// here on the day it is added, without anyone remembering to write a test
// for it — which is the only kind of coverage that keeps working after
// the person who wrote it has moved on.
TEST_CASE("native: no widget anywhere paints truecolor") {
    ui_prefs::publish_theme(theme::native);

    Model m;
    m.d.show_reasoning = true;

    // A user turn, so the user/assistant split renders.
    Message u;
    u.role = Role::User;
    u.id   = MessageId{"u1"};
    u.text = "audit the sandbox and show me the diff";
    m.d.current.messages.push_back(std::move(u));

    // An assistant turn carrying reasoning, prose, AND tool calls — the
    // tool timeline, its body preview, and the diff bands inside it.
    Message a = reasoning_msg(/*done=*/true);
    a.text =
        "Here is the audit.\n\n"
        "- `bwrap` is invoked per tool call\n"
        "- the profile is **fixed** at startup\n\n"
        "```cpp\nint main() { return 0; }\n```\n";

    ToolUse shell;
    shell.id     = ToolCallId{"call_1"};
    shell.name   = ToolName{"shell"};
    shell.args   = json{{"command", "ls -la"}};
    shell.status = ToolUse::Done{.output = "total 4\ndrwxr-xr-x  2 jon jon\n"};
    a.tool_calls.push_back(std::move(shell));

    // A write + a unified diff: the two paths that carried hardcoded
    // GitHub-dark greens and coral reds.
    ToolUse write;
    write.id     = ToolCallId{"call_2"};
    write.name   = ToolName{"write"};
    write.args   = json{{"file_path", "/tmp/x.cpp"}, {"content", "int x = 1;\n"}};
    write.status = ToolUse::Done{.output = "wrote 1 line"};
    a.tool_calls.push_back(std::move(write));

    ToolUse diff;
    diff.id     = ToolCallId{"call_3"};
    diff.name   = ToolName{"git_diff"};
    diff.args   = json{{"path", "."}};
    diff.status = ToolUse::Done{.output =
        "diff --git a/x.cpp b/x.cpp\n"
        "@@ -1,3 +1,4 @@\n"
        " context line\n"
        "-removed line\n"
        "+added line\n"};
    a.tool_calls.push_back(std::move(diff));

    // ...and edit, a THIRD band path with its own colour decisions.
    ToolUse edit;
    edit.id   = ToolCallId{"call_4"};
    edit.name = ToolName{"edit"};
    // Top-level old_text/new_text with NO ```diff fence in the output.
    // That combination is what reaches Kind::EditDiff and push_diff_side's
    // band path. An `edits` ARRAY, or any output carrying a diff fence,
    // renders as Kind::GitDiff instead and never touches this code -- which
    // is why the first version of this test passed with the bug present.
    edit.args = json{{"path", "src/x.cpp"},
                     {"old_text", "int old_line = 1;\nint stays = 2;\n"},
                     {"new_text", "int new_line = 1;\nint stays = 2;\n"}};
    edit.status = ToolUse::Done{.output = "edited src/x.cpp"};
    a.tool_calls.push_back(std::move(edit));

    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};

    // Under native the diff bands must be OFF — that IS the fix, and it is
    // also why the colours inside push_diff_side's band branch stopped
    // mattering: the branch is unreachable here. Assert it, so the next
    // person can tell "the colours are fine" from "nothing ever drew".
    // I spent a while confusing those two.
    CHECK(!maya::ToolBodyPreview::diff_bands_ok_for_test(),
          "native owns no background, so diff bands must degrade to "
          "coloured text. If this flips to true the band branch is live "
          "again and its colours need re-checking.");

    // The status banner in each of its three kinds — the crimson/amber/
    // indigo palette that was hardcoded until this commit. The kind is
    // classified from the status TEXT, so drive it the way the app does
    // rather than setting the enum behind the classifier's back.
    for (const char* status : {"error: the request failed",
                              "retrying (upstream cut off)",
                              "indexing the workspace"}) {
        m.s.status = status;
        m.s.status_until = {};   // no expiry, so status_active() is true
        const auto bad = truecolor_cells(m, 120, 600);
        const std::string msg =
            "a widget painted truecolor under theme::native — native states "
            "no RGB, so this colour is the widget's own palette overriding "
            "the user's. First: "
            + std::string(bad.empty() ? "-" : bad.front());
        CHECK(bad.empty(), msg);
    }
}

// ── Same colour on same colour ─────────────────────────────────────
//
// The sweep above asks "did anyone invent a colour". This asks the
// question that actually matters to someone looking at the screen: "can
// you read it".
//
// They are different questions and the first does not imply the second.
// Fixing the invented-colour bug by moving widgets onto theme slots
// introduced this one: under native the 23 slots collapse onto the 16
// ANSI colours the terminal owns, so `success` and `diff_added` are both
// SGR 32 and a diff band became green text on a green block. Every guard
// in the tree was satisfied — no channel misread, no literal, no
// truecolor — and the text was invisible.
//
// A widget that wants a filled band has to ask whether the theme owns a
// canvas to fill. native does not, by design, so bands degrade to
// coloured TEXT there, which is how diff and status lines have always
// read on a plain terminal.
TEST_CASE("native: a filled chip degrades to coloured text") {
    // The model badge paints the provider name on a filled block — ink from
    // `inverse_text`, canvas from the model family's hue. That works in
    // every scheme and is wrong under native, because native's inverse_text
    // is Default: the terminal's ORDINARY foreground. On Ghostty that made
    // a light-lavender "Anthropic" on a bright-magenta chip. Both colours
    // are real and distinct, so invisible_cells() cannot see it — only the
    // terminal knows they are both light.
    //
    // So the rule is structural, not perceptual: with no canvas to fill,
    // don't fill. Assert the decision rather than the pixels.
    ui_prefs::publish_theme(theme::native);
    CHECK(!ui::theme_owns_canvas(),
          "native states Default for background — it paints no canvas, which "
          "is the whole point of it");

    // Under native nothing may paint a background at all, so no chip, band
    // or tab can put the terminal's own foreground on a coloured block.
    Model m;
    m.d.show_reasoning = true;
    m.d.current.messages.push_back(reasoning_msg(/*done=*/true));
    m.s.phase = phase::Idle{};

    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(120, 400, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);

    std::string first;
    int filled = 0;
    for (int y = 0; y <= canvas.max_content_row(); ++y) {
        for (int x = 0; x < 120; ++x) {
            const auto& cell = canvas.get(x, y);
            const maya::Style& st = pool.get(cell.style_id);
            if (!st.bg.has_value()) continue;
            const LitColor bg = theme::native.resolve(*st.bg);
            // Default bg is "the terminal's own", i.e. no fill at all.
            if (bg.kind() == ColorKind::Default) continue;
            ++filled;
            if (first.empty())
                first = "row " + std::to_string(y) + " col "
                      + std::to_string(x) + " bg=SGR " + bg.fg_sgr();
        }
    }

    const std::string msg =
        "a cell painted a real background under theme::native. native owns "
        "no canvas, so every filled element (chip, band, active tab) must "
        "degrade to coloured text — otherwise its ink is inverse_text, which "
        "under native is just the terminal's ordinary foreground. First: "
        + std::string(first.empty() ? "-" : first);
    CHECK(filled == 0, msg);
}

TEST_CASE("native: no text is painted in its own background colour") {
    ui_prefs::publish_theme(theme::native);

    Model m;
    m.d.show_reasoning = true;

    Message u;
    u.role = Role::User;
    u.id   = MessageId{"u1"};
    u.text = "show me the diff";
    m.d.current.messages.push_back(std::move(u));

    Message a = reasoning_msg(/*done=*/true);
    a.text = "Here is the audit.";

    // The diff paths: a write (all-adds band) and a real unified diff.
    ToolUse write;
    write.id     = ToolCallId{"call_w"};
    write.name   = ToolName{"write"};
    write.args   = json{{"file_path", "/tmp/x.cpp"}, {"content", "int x = 1;\n"}};
    write.status = ToolUse::Done{.output = "wrote 1 line"};
    a.tool_calls.push_back(std::move(write));

    ToolUse diff;
    diff.id     = ToolCallId{"call_d"};
    diff.name   = ToolName{"git_diff"};
    diff.args   = json{{"path", "."}};
    diff.status = ToolUse::Done{.output =
        "diff --git a/x.cpp b/x.cpp\n"
        "@@ -1,3 +1,4 @@\n"
        " context line\n"
        "-removed line\n"
        "+added line\n"};
    a.tool_calls.push_back(std::move(diff));

    // EDIT is a third, separate band path — push_diff_side, not the
    // git_diff parser — and it kept its own hardcoded colours long after
    // the other two were themed. A sweep that renders only write and
    // git_diff passes while the edit band is still green-on-green, which
    // is exactly what happened.
    ToolUse edit;
    edit.id   = ToolCallId{"call_e"};
    edit.name = ToolName{"edit"};
    // Top-level old_text/new_text with NO ```diff fence in the output.
    // That combination is what reaches Kind::EditDiff and push_diff_side's
    // band path. An `edits` ARRAY, or any output carrying a diff fence,
    // renders as Kind::GitDiff instead and never touches this code -- which
    // is why the first version of this test passed with the bug present.
    edit.args = json{{"path", "src/x.cpp"},
                     {"old_text", "int old_line = 1;\nint stays = 2;\n"},
                     {"new_text", "int new_line = 1;\nint stays = 2;\n"}};
    edit.status = ToolUse::Done{.output = "edited src/x.cpp"};
    a.tool_calls.push_back(std::move(edit));

    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};

    // ...and the status banner, whose filled band had the same bug.
    for (const char* status : {"error: the request failed",
                              "retrying (upstream cut off)",
                              "indexing the workspace"}) {
        m.s.status = status;
        m.s.status_until = {};
        const auto bad = invisible_cells(m, 120, 600);
        const std::string msg =
            "text painted in its own background colour under theme::native — "
            "both are valid palette entries, and the result is unreadable. "
            "A band needs a canvas the theme actually owns; native has none, "
            "so it must degrade to coloured text. First: "
            + std::string(bad.empty() ? "-" : bad.front());
        CHECK(bad.empty(), msg);
    }
}
