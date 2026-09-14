#pragma once
// agentty::ui_prefs — resolving preferences into what the renderer uses.
//
// Prefs say what the user WANTS. This says what they GET, which differs
// wherever a preference is "auto" or asks for something the terminal cannot
// do. Keeping the two apart matters: the settings panel shows the wish and
// the resolved value side by side ("auto — truecolor"), which is the only
// way a user can tell a working default from a broken detection.

#include <string_view>

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
[[nodiscard]] inline const maya::Theme* find_scheme(std::string_view name) {
    if (name.empty()) return nullptr;
    for (const auto& s : maya::theme::schemes)
        if (name == s.name) return s.theme;
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

// Why the chosen theme is not in use, for the settings row's detail line.
// Empty when it IS in use.
[[nodiscard]] inline std::string_view theme_override_reason(const Prefs& p,
                                                            const Resolved& r) {
    if (p.theme.empty()) return {};
    if (!find_scheme(p.theme)) return "unknown scheme — using native";
    if (r.theme == &maya::theme::native)
        return "needs 256 colors — using native";
    return {};
}

}  // namespace agentty::ui_prefs
