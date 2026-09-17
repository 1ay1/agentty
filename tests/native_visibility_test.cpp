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

TEST_CASE("native: a rendered reasoning turn paints no truecolor cell") {
    // End-to-end: build a real turn with reasoning, render it under native,
    // and inspect every painted CELL. Under native no slot has channels, so
    // any Rgb foreground or background in the frame was computed from bytes
    // that were not channels. This is the check that would have caught #45
    // as reported.
    ui_prefs::publish_theme(theme::native);

    Model m;
    m.d.show_reasoning = true;
    Message a;
    a.role = Role::Assistant;
    a.id   = MessageId{"a1"};
    a.thinking = "Let me start by exploring the repository structure.\n\n"
                 "The user wants:\n\n"
                 "1. An audit of the implementation\n"
                 "2. A blueprint of what it would take\n\n"
                 "This is a blueprint task, no code editing.";
    a.text = "Here is the audit.";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Idle{};

    constexpr int kW = 100, kH = 400;
    auto root = maya::AppLayout{{
        .thread        = ui::thread_config(m),
        .changes_strip = ui::changes_strip_config(m),
        .composer      = ui::composer_config(m),
        .status_bar    = ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(kW, kH, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::native, true);

    int rgb_cells = 0;
    std::string first;
    const int max_row = canvas.max_content_row();
    for (int y = 0; y <= max_row; ++y) {
        for (int x = 0; x < kW; ++x) {
            const auto& cell = canvas.get(x, y);
            const maya::Style& st = pool.get(cell.style_id);
            for (const auto* c : {&st.fg, &st.bg}) {
                if (!c->has_value()) continue;
                const LitColor lit = theme::native.resolve(**c);
                if (lit.kind() != ColorKind::Rgb) continue;
                ++rgb_cells;
                if (first.empty())
                    first = "row " + std::to_string(y) + " col "
                          + std::to_string(x) + " -> " + lit.fg_sgr();
            }
        }
    }

    CHECK(max_row > 0, "the render produced no rows");
    const std::string msg =
        "cell(s) painted a truecolor value under theme::native — native states "
        "no RGB, so every triple here was computed from bytes that were not "
        "channels (agentty #45). First: "
        + std::string(first.empty() ? "-" : first);
    CHECK(rgb_cells == 0, msg);
}
