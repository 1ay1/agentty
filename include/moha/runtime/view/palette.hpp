#pragma once
// moha::ui — terminal palette using only named ANSI colors.
//
// Named-ANSI-only is a deliberate constraint: the user's terminal theme
// always wins.  We pick semantic names; the terminal decides the hue.

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>

namespace moha::ui {

// ── Semantic palette (named ANSI only — terminal theme wins) ──────────────
// `fg` is the primary body-text color. ANSI 7 ("white") renders as a mid-
// gray in most modern terminal themes (Catppuccin, Solarized, One Dark,
// Gruvbox) — readable, but not the brightest the terminal offers. Prose
// that the user is actually *reading* (their own typed message, the
// assistant's reply paragraphs) maps to ANSI 15 ("bright_white") so it
// pops against the theme's background at maximum contrast. Chrome and
// metadata still use `muted` / `with_dim()` to recede.
inline constexpr auto fg          = maya::Color::bright_white();
inline constexpr auto muted       = maya::Color::bright_black();
inline constexpr auto accent      = maya::Color::magenta();   // brand / Write profile
inline constexpr auto info        = maya::Color::blue();      // Ask profile / threads
inline constexpr auto success     = maya::Color::green();     // accepted / running OK
inline constexpr auto warn        = maya::Color::yellow();    // pending / amber
inline constexpr auto danger      = maya::Color::red();       // errors / rejected
inline constexpr auto highlight   = maya::Color::cyan();      // command palette / mentions

// ── Style presets — terminal default fg unless overridden ─────────────────
inline maya::Style dim()    { return maya::Style{}.with_dim(); }
inline maya::Style bold()   { return maya::Style{}.with_bold(); }
inline maya::Style italic() { return maya::Style{}.with_italic(); }

inline maya::Style fg_of(maya::Color c)         { return maya::Style{}.with_fg(c); }
inline maya::Style fg_bold(maya::Color c)       { return maya::Style{}.with_fg(c).with_bold(); }

// `fg_dim` returns "dim color" — but `with_dim()` on an already-muted
// color (bright_black / gray) collapses below the readable floor on
// dark / low-contrast themes (true-black backgrounds, OLED palettes,
// some Solarized variants). The intent of `fg_dim(muted)` is "subdued
// secondary text," and bright_black ALONE already carries that role
// on every reasonable theme — stacking the SGR `dim` attribute on top
// just trades readability for nothing. So suppress the `with_dim()`
// when the color is bright_black; keep it for everything else, where
// dimming a bright color is exactly the meaningful signal we want
// (a muted form of the brand color, etc.).
inline maya::Style fg_dim(maya::Color c) {
    const bool is_already_muted =
        c.kind() == maya::Color::Kind::Named
        && c.index() == static_cast<uint8_t>(maya::AnsiColor::BrightBlack);
    return is_already_muted
        ? maya::Style{}.with_fg(c)
        : maya::Style{}.with_fg(c).with_dim();
}
inline maya::Style fg_italic(maya::Color c)     { return maya::Style{}.with_fg(c).with_italic(); }

} // namespace moha::ui
