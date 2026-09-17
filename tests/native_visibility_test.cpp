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

#include "agentty/domain/ui_theme.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

using namespace agentty;
using namespace maya;

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
