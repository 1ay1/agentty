// TERM and the terminal's own colours — issue #37.
//
// The report was "horrible look and feel in non-black terminals, almost
// unusable", from a user with a light-grey background and black text, plus
// three quieter complaints in the same breath: TERM is not respected, there
// is no black-and-white option, and agentty assumes a gnome-terminal-ish
// emulator.
//
// The colour half is answered by theme::native being the DEFAULT — it states
// no literal colours at all, so a fresh install paints with SGR 39/49 (the
// terminal's own ink and background) and the sixteen NAMED ansi slots, which
// are indices into the palette the USER configured. Nothing maya picked.
//
// The TERM half needed a real fix: `TERM=dumb` was honoured by the colour
// tier and nowhere else, so agentty went monochrome and then wrapped every
// frame in DEC private mode 2026 anyway. "Unknown private modes are no-ops"
// is true of a terminal with a DEC parser; a dumb terminal has none, so the
// bytes land as literal garbage.

#include "agtest.hpp"

#include "agentty/domain/ui_theme.hpp"

#include <maya/style/schemes.hpp>
#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/ansi.hpp>

#include <cstdlib>
#include <print>
#include <string>

namespace {

// Scoped setenv/unsetenv, so a case cannot leak into the next.
struct ScopedEnv {
    std::string key;
    bool had_old = false;
    std::string old;
    ScopedEnv(const char* k, const char* v) : key(k) {
        if (const char* prev = std::getenv(k)) { had_old = true; old = prev; }
        if (v) ::setenv(k, v, 1); else ::unsetenv(k);
    }
    ~ScopedEnv() {
        if (had_old) ::setenv(key.c_str(), old.c_str(), 1);
        else         ::unsetenv(key.c_str());
    }
};

}  // namespace

TEST_CASE("issue 37: the default theme states no colours of its own") {
    // The heart of it. Every slot must resolve to either the terminal's
    // default (SGR 39/49) or a NAMED ansi index — never a literal RGB that
    // was authored against somebody else's background.
    //
    // A single hardcoded hue here is the whole bug: pick #565F89 for muted
    // text and it reads on black and vanishes on light grey, which is
    // precisely the screenshot on the issue.
    int rgb_slots = 0;
    int checked = 0;
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(maya::ThemeSlot::Overlay); ++i) {
        const auto slot = static_cast<maya::ThemeSlot>(i);
        const maya::LitColor c =
            maya::theme::native.resolve(maya::Color::slot(slot));
        // Named and Default are both "the user's own palette". Rgb/Indexed
        // would be maya imposing a colour.
        if (c.kind() == maya::ColorKind::Rgb || c.kind() == maya::ColorKind::Indexed)
            ++rgb_slots;
        ++checked;
    }
    CHECK(checked == static_cast<int>(maya::kThemeSlotCount));
    CHECK(rgb_slots == 0);
}

TEST_CASE("issue 37: native paints no background, so a light terminal stays light") {
    // owns_canvas is the predicate a host uses to decide whether to fill the
    // viewport. native must answer NO — filling it is what turns a light
    // terminal into a dark rectangle with unreadable text on it.
    CHECK(!maya::theme::owns_canvas(maya::theme::native));
    CHECK(maya::theme::native.background.kind() == maya::ColorKind::Default);

    // A scheme the user PICKED is allowed to own the canvas: they asked for
    // it, and it states a matching foreground. The distinction is consent.
    const maya::Theme* dracula = nullptr;
    for (const auto& s : maya::theme::schemes)
        if (std::string_view{s.name} == "Dracula") dracula = s.theme;
    REQUIRE(dracula != nullptr);
    CHECK(maya::theme::owns_canvas(*dracula));
}

TEST_CASE("issue 37: TERM=dumb means no escape sequences, not just no colour") {
    // This is what "doesn't respect TERM" meant in practice. The colour tier
    // honoured dumb; nothing else did.
    {
        ScopedEnv term{"TERM", "dumb"};
        CHECK(maya::theme::terminal_is_dumb());
        CHECK(maya::theme::detect_tier(/*tty=*/true) == maya::theme::ColorTier::Mono);
        // A dumb terminal cannot support a DEC private mode, whatever the
        // rest of the environment claims.
        CHECK(!maya::ansi::env_supports_synchronized_output());
    }
    // ...and a stale force flag must not resurrect escape output for a user
    // who explicitly asked for plain text.
    {
        ScopedEnv term{"TERM", "dumb"};
        ScopedEnv force{"MAYA_FORCE_SYNC", "1"};
        CHECK(!maya::ansi::env_supports_synchronized_output());
    }
    // A normal terminal is unaffected by any of this.
    {
        ScopedEnv term{"TERM", "xterm-256color"};
        CHECK(!maya::theme::terminal_is_dumb());
    }
}

TEST_CASE("issue 37: a black-and-white terminal is a supported request") {
    // "What if I need or want black&white terminal?" — three ways to ask,
    // all of which must reach Mono, because the answer cannot depend on
    // which spelling the user knows.
    {
        ScopedEnv term{"TERM", "dumb"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Mono);
    }
    {
        ScopedEnv term{"TERM", "xterm-256color"};
        ScopedEnv no_color{"NO_COLOR", "1"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Mono);
    }
    {
        ScopedEnv term{"TERM", "xterm-256color"};
        ScopedEnv forced{"MAYA_COLOR", "none"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Mono);
    }
    // Piping to a file is the fourth: a redirected log should be text, not
    // escape soup.
    {
        ScopedEnv term{"TERM", "xterm-256color"};
        ScopedEnv no_color{"NO_COLOR", nullptr};
        ScopedEnv forced{"MAYA_COLOR", nullptr};
        CHECK(maya::theme::detect_tier(/*tty=*/false) == maya::theme::ColorTier::Mono);
    }
}

TEST_CASE("issue 37: capability is inferred conservatively, never assumed richest") {
    // "rxvt, different versions of xterm, gnome-terminals, konsole — all are
    // distinct in minor details." maya cannot consult termcap, so the
    // question is whether its inference is DEFENSIBLE per family, not
    // whether it is uniformly pessimistic.
    ScopedEnv ct{"COLORTERM", nullptr};
    ScopedEnv no_color{"NO_COLOR", nullptr};
    ScopedEnv forced{"MAYA_COLOR", nullptr};
    ScopedEnv prog{"TERM_PROGRAM", nullptr};
    ScopedEnv wt{"WT_SESSION", nullptr};

    // A bare VT-era TERM gets the sixteen colours every terminal has had
    // since the VT100's descendants — and NOTHING more. This is the case the
    // reporter named explicitly ("setting TERM=vt100 usually solves
    // problems"), so it must not escalate.
    {
        ScopedEnv term{"TERM", "vt100"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Ansi16);
    }
    // The xterm/screen/tmux/rxvt/linux family gets INDEXED colour, not
    // truecolor. Indexed SGR is universally safe there, and quantising to
    // sixteen would collapse tuned greys into primaries — but 38;2 would be
    // a claim the family does not support.
    for (const char* t : {"rxvt", "xterm", "linux", "screen"}) {
        ScopedEnv term{"TERM", t};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Ansi256);
    }
    // A terminal that IS truecolor is believed on its name, because that is
    // how the capability survives an ssh hop where COLORTERM does not.
    {
        ScopedEnv term{"TERM", "konsole"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::TrueColor);
    }
    // An explicit capability claim always wins — a claim beats a guess.
    {
        ScopedEnv term{"TERM", "xterm-256color"};
        ScopedEnv claim{"COLORTERM", "truecolor"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::TrueColor);
    }
    // And whatever the tier, the USER's override is above all of it.
    {
        ScopedEnv term{"TERM", "konsole"};
        ScopedEnv override_{"MAYA_COLOR", "16"};
        CHECK(maya::theme::detect_tier(true) == maya::theme::ColorTier::Ansi16);
    }
}

TEST_CASE("issue 37: a low colour tier falls back to native, not a bad approximation") {
    // A named scheme states literal RGB including a background. Quantised to
    // 16 colours that becomes mud — and mud with a background is exactly the
    // unreadable screenshot. Below 256 colours the honest answer is the
    // user's own palette.
    agentty::ui_prefs::Prefs p;
    p.theme = "Dracula";
    p.tier  = agentty::ui_prefs::ColorTier::Ansi16;
    const auto r = agentty::ui_prefs::resolve(p, /*tty=*/true);
    CHECK(r.theme == &maya::theme::native);
}

TEST_CASE("theming: every scheme in the picker is findable by name") {
    // find_scheme() binary-searches schemes[], which is only correct while
    // that table is sorted. If it ever is not, the search MISSES a scheme
    // that is present — and the symptom is not a crash but "unknown scheme
    // — using native" for a theme the user can see listed in front of them.
    //
    // schemes.hpp static_asserts the ordering, so this is the behavioural
    // half: every name the picker can show must round-trip through the
    // lookup the picker uses.
    int checked = 0;
    for (const auto& s : maya::theme::schemes) {
        const maya::Theme* found = agentty::ui_prefs::find_scheme(s.name);
        if (found != s.theme) {
            CHECK(std::string_view{s.name} == "<round-tripped>");  // names it
            break;
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(std::size(maya::theme::schemes)));
    CHECK(checked > 600);

    // A name that is not there is a miss, not a neighbour. A sloppy binary
    // search returns the insertion point's entry, which would silently hand
    // back the alphabetically-adjacent theme.
    CHECK(agentty::ui_prefs::find_scheme("Dracul")  == nullptr);
    CHECK(agentty::ui_prefs::find_scheme("Draculaa") == nullptr);
    CHECK(agentty::ui_prefs::find_scheme("")         == nullptr);
    CHECK(agentty::ui_prefs::find_scheme("zzzzzzz")  == nullptr);
}

TEST_CASE("theming: the Background preference actually does something") {
    // It used to do nothing at all. Polarity was detected, resolved,
    // persisted and displayed — a complete round trip — while no consumer
    // read it, and the settings row's help claimed it was "consulted by
    // schemes that have both", which no scheme is. A setting that changes
    // nothing is worse than a missing one: the user concludes the thing
    // they wanted is impossible.
    agentty::ui_prefs::Prefs p;
    p.theme    = "Dracula";                        // a dark scheme
    p.tier     = agentty::ui_prefs::ColorTier::TrueColor;
    p.polarity = agentty::ui_prefs::Polarity::Light;

    const auto r = agentty::ui_prefs::resolve(p, /*tty=*/true);
    REQUIRE(r.theme != nullptr);
    CHECK(agentty::ui_prefs::scheme_is_light(*r.theme) == std::optional{false});
    CHECK(!agentty::ui_prefs::theme_override_reason(p, r).empty());

    // A NOTE, not an override: the user picked this scheme on this terminal
    // and is allowed to mean it. Swapping their theme because a heuristic
    // disagreed is the kind of guess this whole pane exists to avoid.
    CHECK(r.theme != &maya::theme::native);

    // Matching polarity is unremarkable and says nothing.
    agentty::ui_prefs::Prefs q = p;
    q.polarity = agentty::ui_prefs::Polarity::Dark;
    const auto rq = agentty::ui_prefs::resolve(q, /*tty=*/true);
    CHECK(agentty::ui_prefs::theme_override_reason(q, rq).empty());

    // And a light scheme on a light terminal is equally quiet — the check
    // is symmetric, not "dark is suspicious".
    agentty::ui_prefs::Prefs l;
    l.theme    = "Catppuccin Latte";               // a light scheme
    l.tier     = agentty::ui_prefs::ColorTier::TrueColor;
    l.polarity = agentty::ui_prefs::Polarity::Light;
    const auto rl = agentty::ui_prefs::resolve(l, /*tty=*/true);
    REQUIRE(rl.theme != nullptr);
    CHECK(agentty::ui_prefs::scheme_is_light(*rl.theme) == std::optional{true});
    CHECK(agentty::ui_prefs::theme_override_reason(l, rl).empty());
}

// native states a Default background — SGR 49, the terminal's own canvas.
// It therefore has NO polarity of its own, and must not claim one: it used
// to project Default through to_rgb(), get white, and report itself as a
// light scheme with luminance 1.0. Any caller comparing that against a
// detected polarity got a confident wrong answer on every dark terminal.
TEST_CASE("issue 37: native claims no polarity, because it paints no canvas") {
    CHECK(!agentty::ui_prefs::scheme_is_light(maya::theme::native).has_value());

    // And so it never triggers the mismatch note, on either polarity.
    for (auto pol : {agentty::ui_prefs::Polarity::Light,
                     agentty::ui_prefs::Polarity::Dark}) {
        agentty::ui_prefs::Prefs p;
        p.theme    = "";            // no scheme named => native
        p.tier     = agentty::ui_prefs::ColorTier::TrueColor;
        p.polarity = pol;
        const auto r = agentty::ui_prefs::resolve(p, /*tty=*/true);
        CHECK(agentty::ui_prefs::theme_override_reason(p, r).empty());
    }
}
