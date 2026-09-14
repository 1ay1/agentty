#pragma once
// agentty::ui_prefs — every knob that changes how agentty LOOKS, in one
// value type.
//
// ── Why this is its own concern ──────────────────────────────────────────
//
// Appearance is the one part of the config that belongs to the PERSON, not
// the project. A permission profile is a property of the repo you are in;
// a light terminal is a property of your eyes and your office. So these
// live in the user store and follow you between checkouts, and nothing
// here is ever written to a project .agentty/.
//
// ── Why every field has an explicit "auto" ───────────────────────────────
//
// Two of these — the color tier and the background polarity — are things
// agentty can usually DETECT. Detection is right almost always and wrong
// unrecoverably: a terminal that lies about COLORTERM, or one that never
// set COLORFGBG, leaves a user staring at an unreadable screen with no way
// to say "no, it's light". Auto is the default and the override is the
// escape hatch, which is the whole of agentty issue #37.

#include <cstdint>
#include <string>
#include <string_view>

namespace agentty::ui_prefs {

// ── Theme ────────────────────────────────────────────────────────────────

// Which palette paints the UI.
//
// Empty means NATIVE: agentty states no colors of its own and the user's
// own terminal palette comes through (maya::theme::native). That is the
// default because it is the only choice that is correct on a light
// terminal, a dark one, and a monochrome one alike.
//
// Any other value names one of maya's built-in schemes ("Dracula",
// "Gruvbox Dark", …). Choosing one is the user saying "paint it like this",
// and agentty then owns the whole surface including the background.
using ThemeName = std::string;

// What agentty may emit.
//
// Auto runs maya's detection stack (NO_COLOR, TERM, COLORTERM, tty). The
// rest force a tier, for a terminal that misreports — or for someone who
// simply wants a black-and-white screen, which Mono gives them.
enum class ColorTier : std::uint8_t { Auto, TrueColor, Ansi256, Ansi16, Mono };

// Whether the terminal is light or dark.
//
// Only consulted when a named theme has both variants; `native` does not
// care, which is the point of it. Auto reads COLORFGBG and, failing that,
// leaves it unknown rather than guessing.
enum class Polarity : std::uint8_t { Auto, Dark, Light };

// ── Density ──────────────────────────────────────────────────────────────

// How tall a panel's scrollable body may be.
//
// The shared default is 14 rows, chosen so a picker fits a short terminal.
// On a tall one that is needlessly cramped — several stats tabs scroll for
// want of six rows — so this raises the ceiling. It is a MAXIMUM: the panel
// still clamps to what the terminal actually has.
enum class Density : std::uint8_t { Compact, Normal, Roomy };

[[nodiscard]] constexpr int viewport_rows(Density d) noexcept {
    switch (d) {
        case Density::Compact: return 10;
        case Density::Normal:  return 14;
        case Density::Roomy:   return 22;
    }
    return 14;
}

// ── Motion ───────────────────────────────────────────────────────────────

// Whether anything moves.
//
// Off is an accessibility setting before it is a preference: vestibular
// disorders make a typewriter reveal genuinely unpleasant, and a spinner
// on a slow SSH link is just noise on the wire. Off makes streamed text
// appear in whole lines and replaces every spinner with a static mark.
enum class Motion : std::uint8_t { Full, Reduced, Off };

// ── Chrome ───────────────────────────────────────────────────────────────

// How much of a tool's output is shown before you ask for more.
enum class ToolOutput : std::uint8_t { Collapsed, Preview, Full };

// Whether the model's reasoning is shown.
enum class Thinking : std::uint8_t { Shown, Collapsed, Hidden };

// Whether turns carry a time, and in what form.
enum class Timestamps : std::uint8_t { Off, Relative, Absolute };

// ── The whole of it ──────────────────────────────────────────────────────

struct Prefs {
    ThemeName  theme;                              // empty = native
    ColorTier  tier        = ColorTier::Auto;
    Polarity   polarity    = Polarity::Auto;

    Density    density     = Density::Normal;
    // Reading measure for assistant prose, in columns. Long lines are hard
    // to track back to the next line's start; 0 means "no cap".
    int        prose_width = 0;
    bool       compact_turns = false;

    Motion     motion      = Motion::Full;

    bool       syntax      = true;
    ToolOutput tool_output = ToolOutput::Preview;
    Thinking   thinking    = Thinking::Collapsed;
    Timestamps timestamps  = Timestamps::Off;

    bool operator==(const Prefs&) const = default;
};

// ── Labels ───────────────────────────────────────────────────────────────
//
// One spelling per value, used by the settings rows AND anything else that
// reports the state, so the two can never drift.

[[nodiscard]] constexpr std::string_view label(ColorTier t) noexcept {
    switch (t) {
        case ColorTier::Auto:      return "auto";
        case ColorTier::TrueColor: return "truecolor";
        case ColorTier::Ansi256:   return "256 colors";
        case ColorTier::Ansi16:    return "16 colors";
        case ColorTier::Mono:      return "monochrome";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Polarity p) noexcept {
    switch (p) {
        case Polarity::Auto:  return "auto";
        case Polarity::Dark:  return "dark";
        case Polarity::Light: return "light";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Density d) noexcept {
    switch (d) {
        case Density::Compact: return "compact";
        case Density::Normal:  return "normal";
        case Density::Roomy:   return "roomy";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Motion m) noexcept {
    switch (m) {
        case Motion::Full:    return "full";
        case Motion::Reduced: return "reduced";
        case Motion::Off:     return "off";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(ToolOutput t) noexcept {
    switch (t) {
        case ToolOutput::Collapsed: return "collapsed";
        case ToolOutput::Preview:   return "preview";
        case ToolOutput::Full:      return "full";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Thinking t) noexcept {
    switch (t) {
        case Thinking::Shown:     return "shown";
        case Thinking::Collapsed: return "collapsed";
        case Thinking::Hidden:    return "hidden";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view label(Timestamps t) noexcept {
    switch (t) {
        case Timestamps::Off:      return "off";
        case Timestamps::Relative: return "relative";
        case Timestamps::Absolute: return "absolute";
    }
    return "?";
}

// ── Cycling ──────────────────────────────────────────────────────────────
//
// Enter on a row advances it. Every enum wraps, so a row is a loop the user
// can always get back around — never a dead end at the last value.

template <typename E, std::uint8_t N>
[[nodiscard]] constexpr E cycle(E v) noexcept {
    return static_cast<E>((static_cast<std::uint8_t>(v) + 1) % N);
}

[[nodiscard]] constexpr ColorTier  next(ColorTier v)  noexcept { return cycle<ColorTier, 5>(v); }
[[nodiscard]] constexpr Polarity   next(Polarity v)   noexcept { return cycle<Polarity, 3>(v); }
[[nodiscard]] constexpr Density    next(Density v)    noexcept { return cycle<Density, 3>(v); }
[[nodiscard]] constexpr Motion     next(Motion v)     noexcept { return cycle<Motion, 3>(v); }
[[nodiscard]] constexpr ToolOutput next(ToolOutput v) noexcept { return cycle<ToolOutput, 3>(v); }
[[nodiscard]] constexpr Thinking   next(Thinking v)   noexcept { return cycle<Thinking, 3>(v); }
[[nodiscard]] constexpr Timestamps next(Timestamps v) noexcept { return cycle<Timestamps, 3>(v); }

}  // namespace agentty::ui_prefs
