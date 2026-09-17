#pragma once
// agentty::ui_prefs — resolving preferences into what the renderer uses.
//
// Prefs say what the user WANTS. This says what they GET, which differs
// wherever a preference is "auto" or asks for something the terminal cannot
// do. Keeping the two apart matters: the settings panel shows the wish and
// the resolved value side by side ("auto — truecolor"), which is the only
// way a user can tell a working default from a broken detection.

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string_view>

#include <maya/app/app.hpp>       // app_set_theme — the renderer-side sink
#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>

#include "agentty/domain/ui_prefs.hpp"

namespace agentty::ui_prefs {

// What the terminal will actually get, after detection and clamping.
struct Resolved {
    const maya::Theme* theme = &maya::theme::native;
    maya::theme::ColorTier tier = maya::theme::ColorTier::Ansi16;
    maya::theme::Polarity  polarity = maya::theme::Polarity::Unknown;
    // True when the value came from detection rather than the user. The
    // panel needs this to render "auto — truecolor" honestly.
    bool tier_detected = true;
    bool polarity_detected = true;
};

// Look up a built-in scheme by name. Null when the name is unknown, which
// is not an error: a config naming a scheme this build does not carry
// should fall back to native rather than refuse to start.
//
// BINARY search, not linear. resolve() calls this and view() calls
// resolve() every frame, so the scan ran once per frame over the whole
// table — fine at 57 schemes, 1.25 us/frame at 615, for an answer that
// cannot change between frames. schemes[] is emitted sorted and
// static_asserts that it is, so the search is correct by construction
// rather than by convention.
[[nodiscard]] inline const maya::Theme* find_scheme(std::string_view name) {
    if (name.empty()) return nullptr;
    const auto* first = std::begin(maya::theme::schemes);
    const auto* last  = std::end(maya::theme::schemes);
    const auto* it = std::lower_bound(
        first, last, name,
        [](const maya::theme::NamedTheme& s, std::string_view n) {
            return std::string_view{s.name} < n;
        });
    if (it != last && name == std::string_view{it->name}) return it->theme;
    return nullptr;
}

[[nodiscard]] inline maya::theme::ColorTier to_maya(ColorTier t) {
    switch (t) {
        case ColorTier::TrueColor: return maya::theme::ColorTier::TrueColor;
        case ColorTier::Ansi256:   return maya::theme::ColorTier::Ansi256;
        case ColorTier::Ansi16:    return maya::theme::ColorTier::Ansi16;
        case ColorTier::Mono:      return maya::theme::ColorTier::Mono;
        case ColorTier::Auto:      break;
    }
    return maya::theme::ColorTier::Ansi16;
}

// Resolve. `tty` is whether stdout is a terminal — the caller owns that
// question because it is a platform fact, not a preference.
[[nodiscard]] inline Resolved resolve(const Prefs& p, bool tty) {
    Resolved r;

    // Tier: detection unless overridden.
    if (p.tier == ColorTier::Auto) {
        r.tier = maya::theme::detect_tier(tty);
        r.tier_detected = true;
    } else {
        r.tier = to_maya(p.tier);
        r.tier_detected = false;
    }

    // Polarity: COLORFGBG unless overridden. Stays Unknown when nothing
    // said — a guess here is the bug this whole feature exists to fix.
    if (p.polarity == Polarity::Auto) {
        r.polarity = maya::theme::detect_polarity();
        r.polarity_detected = true;
    } else {
        r.polarity = (p.polarity == Polarity::Light)
                         ? maya::theme::Polarity::Light
                         : maya::theme::Polarity::Dark;
        r.polarity_detected = false;
    }

    // Theme. A named scheme states literal RGB including a background, so
    // it only makes sense where the terminal can show it: below 256 colors
    // the scheme's palette would be quantised into mud, and native — which
    // is just the user's own sixteen — is strictly better. So a low tier
    // falls back rather than rendering a bad approximation of Dracula.
    const maya::Theme* named = find_scheme(p.theme);
    const bool can_paint = r.tier == maya::theme::ColorTier::TrueColor
                        || r.tier == maya::theme::ColorTier::Ansi256;
    r.theme = (named && can_paint) ? named : &maya::theme::native;
    return r;
}

// Is this theme's own background light?
//
// A scheme states literal RGB, so its polarity is a FACT about it rather
// than a preference — which is what lets resolve() notice when the scheme a
// user picked disagrees with the terminal they are on.
//
// ── Only when the background actually has channels ────────────────────
//
// std::nullopt means "this theme has no opinion": theme::native states its
// background as Default (SGR 49), and a Named/Indexed background is a
// palette slot whose RGB only the terminal knows. Projecting either through
// to_rgb() invents an answer — it returns white for Default, so native used
// to report itself as a LIGHT scheme with luminance 1.0, and every caller
// comparing that against a detected polarity got a confident wrong answer.
//
// A theme that owns no canvas cannot clash with the terminal's, so "no
// opinion" is the truthful result and callers skip the comparison.
[[nodiscard]] inline std::optional<bool>
scheme_is_light(const maya::Theme& t) noexcept {
    const maya::LitColor bg = t.background;
    if (!bg.has_channels()) return std::nullopt;
    return (0.2126 * bg.r() + 0.7152 * bg.g() + 0.0722 * bg.b())  // has_channels
           / 255.0 > 0.5;
}

// Why the chosen theme is not in use, or a caveat about it, for the
// settings row's detail line. Empty when it is in use and unremarkable.
[[nodiscard]] inline std::string_view theme_override_reason(const Prefs& p,
                                                            const Resolved& r) {
    if (p.theme.empty()) return {};
    if (!find_scheme(p.theme)) return "unknown scheme — using native";
    if (r.theme == &maya::theme::native)
        return "needs 256 colors — using native";

    // Polarity mismatch. A scheme that owns the canvas paints its own
    // background over the terminal's, so a dark scheme on a light terminal
    // is not "wrong" — it works, it just makes the surrounding terminal
    // chrome (tab bar, split borders, anything agentty does not paint) clash
    // with the pane.
    //
    // This is the ONLY thing the Background preference can honestly affect.
    // It used to affect nothing at all: it was detected, resolved,
    // persisted, and displayed — a complete round trip — while no consumer
    // ever read it, and the row's own help promised it was "consulted by
    // schemes that have both", which no scheme is. A setting that changes
    // nothing is worse than a missing one, because the user reasonably
    // concludes the thing they wanted is impossible.
    //
    // A NOTE, not an override: the user picked this scheme on this terminal
    // and is allowed to mean it. Silently swapping their theme because a
    // heuristic disagreed is exactly the kind of guess issue #37 is about.
    if (r.polarity != maya::theme::Polarity::Unknown) {
        const bool term_light = r.polarity == maya::theme::Polarity::Light;
        // nullopt = the scheme paints no canvas of its own (native), so
        // there is nothing for the terminal's polarity to disagree with.
        if (const auto light = scheme_is_light(*r.theme);
            light && *light != term_light)
            return term_light ? "dark scheme on a light terminal"
                              : "light scheme on a dark terminal";
    }
    return {};
}

// ── The resolved theme, readable from the view ───────────────────────────
//
// Same seam, same reason as the prefs above. maya publishes its own theme
// slot so the RENDERER can paint with it, but agentty's view builders pick
// colours while BUILDING the tree — `fg_of(accent)` runs long before any
// renderer sees a Theme — so they need the palette here, at build time.
//
// Without this, every colour in the app was an `inline constexpr` ANSI
// literal and choosing a theme changed nothing you could see. The theme
// browser repainted its own swatches from the scheme table and the other
// 493 call sites went on painting bright_white.

namespace detail {
// A POINTER, not a copy: every Theme lives in a constexpr table with static
// storage, so the slot is just a re-seated view of one of them — the read
// side is a plain atomic load rather than a locked copy of 24 Colors on
// every styled span. (Contrast Prefs, which holds a std::string and so
// needs the mutex above.)
inline std::atomic<const maya::Theme*>& theme_atom() noexcept {
    static std::atomic<const maya::Theme*> t{&maya::theme::native};
    return t;
}
}  // namespace detail

// Publish the theme in force — to BOTH sinks, always.
//
// There are two, and they feed different consumers:
//
//   • this atom       — agentty's `ui::` tokens, read while BUILDING an
//                       Element tree (fg(), muted, accent …).
//   • app_set_theme   — maya's renderer slot, and via on_theme_changed
//                       every PROJECTED palette: markdown's 37 flat
//                       colours, the cross-frame component cache,
//                       StylePool's SGR cache.
//
// They used to be two calls at the call site, which is a rule nobody can
// keep: the appearance reducer made one of them and re-sealed the whole
// transcript against a palette maya had not been told about yet, so a
// dark→light swap left every older turn in the outgoing theme's ink. One
// entry point means the pair cannot come apart — the second sink is not
// something a caller can forget, because there is no call to omit.
//
// Idempotent and cheap: the atom is a pointer store and app_set_theme
// compares by value and returns early when nothing moved, which is the
// common case (view() re-publishes the same theme every frame so `auto`
// can follow a tmux detach or an ssh hop).
inline void publish_theme(const maya::Theme& t) noexcept {
    detail::theme_atom().store(&t, std::memory_order_relaxed);
    maya::app_set_theme(t);
}

// The theme in force. Native until the first publish_theme(), so a
// consumer that runs before the first frame (a unit test building an
// element directly) gets the safe terminal-native palette rather than a
// null deref.
[[nodiscard]] inline const maya::Theme& theme() noexcept {
    return *detail::theme_atom().load(std::memory_order_relaxed);
}

}  // namespace agentty::ui_prefs
