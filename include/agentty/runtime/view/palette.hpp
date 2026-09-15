#pragma once
// agentty::ui — the semantic palette, resolved against the live theme.
//
// Every colour in agentty comes from here, and every token here is a read
// of the theme the user chose in Settings → Appearance. The default theme
// is `native`, which states only terminal-default and the sixteen named
// ANSI colours — so out of the box the old constraint still holds exactly:
// we pick semantic names, the user's terminal decides the hue.

#include <maya/style/color.hpp>
#include <maya/style/style.hpp>

#include "agentty/domain/ui_theme.hpp"        // ui_prefs::theme()
#include "agentty/runtime/panel/palette.hpp"   // PaletteContext, Command
#include "agentty/runtime/model.hpp"            // Model

namespace agentty::ui {

// ── Semantic palette (named ANSI only — terminal theme wins) ──────────────
//
// Two layers below: legacy "brand" names (back-compat with existing widget
// configs) and a new token layer organised by axis. The discipline is
// "one hue = one axis": status colors (green/yellow/red) ONLY mean status
// (ok/warn/error), code-reference cyan ONLY means "filesystem path or
// code identifier," brand magenta ONLY means agentty identity (Write
// profile, queue chips), info blue ONLY means information / context-shift
// (threads, vcs). Tool categories compress to a tonal palette so a long
// run of inspect-class tools reads as a calm gray list, with magenta
// edits and cyan bashes standing out as the meaningful actions.
//
// ── Why these are functions, not constants ───────────────────────────────
//
// They were `inline constexpr` ANSI literals, and that made the entire
// Appearance theme picker a lie: you could browse 57 schemes, watch the
// name change, and the UI never moved, because every colour in the app was
// baked at compile time. Choosing Dracula repainted nothing.
//
// A theme is judged by LOOKING at it, so the tokens have to be reads of the
// theme in force rather than literals. `ui_prefs::theme()` is one relaxed
// atomic load of a pointer into a constexpr table — cheap enough to sit on
// the hot path of every styled span, and re-seated once per frame by view().
//
// The NAMES and the call sites stay EXACTLY as they were — `fg_of(muted)`
// still reads `fg_of(muted)`. Each token is a tiny proxy that converts to a
// Color by reading the live theme, so ~493 use sites needed no edit at all.
// That matters beyond churn: several tokens (`info`, `success`, `fg`) share
// a name with ordinary locals in these files, so a mechanical rename would
// have been a silent-breakage machine.
//
// The axis discipline below (text tiers / status / code / role) is
// unchanged — each token still means one thing, it just sources its hue
// from the active scheme instead of from a literal.
//
// `native` maps every slot back to the terminal's OWN sixteen colours, so
// the default behaviour after this change is exactly the behaviour before
// it: the user's palette, not ours.

namespace detail {

// A named slot in the live theme. Converts to Color on use, which is what
// makes it substitutable for the constant it replaced.
struct Slot {
    maya::Color (*read)() noexcept;
    [[nodiscard]] operator maya::Color() const noexcept { return read(); }  // NOLINT(google-explicit-constructor)
    [[nodiscard]] maya::Color operator()() const noexcept { return read(); }
};

[[nodiscard]] inline const maya::Theme& thm() noexcept { return ui_prefs::theme(); }

}  // namespace detail

#define AGENTTY_THEME_SLOT(field) \
    ::agentty::ui::detail::Slot{ []() noexcept { \
        return ::agentty::ui::detail::thm().field; } }

// Prose that the user is actually READING maps to the theme's `text` slot,
// which native resolves to the terminal's default foreground — maximum
// contrast against whatever background they actually have, rather than a
// literal bright_white that is invisible on a light terminal. Chrome and
// metadata still use `muted` / `with_dim()` to recede.
inline constexpr auto fg        = AGENTTY_THEME_SLOT(text);
inline constexpr auto muted     = AGENTTY_THEME_SLOT(muted);
inline constexpr auto accent    = AGENTTY_THEME_SLOT(accent);   // brand / Write profile
inline constexpr auto info      = AGENTTY_THEME_SLOT(info);     // Ask profile / threads
inline constexpr auto success   = AGENTTY_THEME_SLOT(success);  // accepted / running OK
inline constexpr auto warn      = AGENTTY_THEME_SLOT(warning);  // pending / amber
inline constexpr auto danger    = AGENTTY_THEME_SLOT(error);    // errors / rejected
inline constexpr auto highlight = AGENTTY_THEME_SLOT(primary);  // command palette / mentions

// ── Token layer (axis-disciplined). Prefer these in new code. ────────────

// Text hierarchy — three tiers from primary prose down to chrome.
inline constexpr auto text_primary   = AGENTTY_THEME_SLOT(text);       // prose, headlines
inline constexpr auto text_secondary = AGENTTY_THEME_SLOT(secondary);  // mid-tone metadata
inline constexpr auto text_tertiary  = AGENTTY_THEME_SLOT(muted);      // footers, hints, blanks
// Ink for text sitting ON a filled badge — the theme's own answer to
// "what reads against my accent colours", rather than a literal black that
// disappears the moment a scheme's badge hue is dark.
inline constexpr auto text_inverse   = AGENTTY_THEME_SLOT(inverse_text);

// Status — severity / outcome ONLY. Never a category color.
inline constexpr auto status_ok    = AGENTTY_THEME_SLOT(success);
inline constexpr auto status_info  = AGENTTY_THEME_SLOT(info);
inline constexpr auto status_warn  = AGENTTY_THEME_SLOT(warning);
inline constexpr auto status_error = AGENTTY_THEME_SLOT(error);

// Code references — file paths, identifiers, command args.
inline constexpr auto code_path = AGENTTY_THEME_SLOT(link);  // file paths, identifiers
inline constexpr auto code_text = AGENTTY_THEME_SLOT(info);  // code-block body content

// Role / identity — persistent identifier colors. One hue per role.
inline constexpr auto role_brand     = AGENTTY_THEME_SLOT(accent);   // brand, Write profile
inline constexpr auto role_brand_alt = AGENTTY_THEME_SLOT(primary);  // queue chips, alt brand
inline constexpr auto role_info      = AGENTTY_THEME_SLOT(info);     // Ask profile, threads, vcs

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

// ── Palette row visibility ─────────────────────────────────────
//
// Rows are GATED — Review/Accept-all need pending changes, Run-code-block a
// fenced reply, Update a release — and THREE places must agree on which are
// visible: the VIEW that renders the list, the REDUCER that resolves a cursor
// to a command, and back_to() restoring a cursor after Esc. When they
// disagree the palette shows one list while the cursor indexes another, so
// Enter fires a neighbour and Esc restores the wrong row.
//
// It was written out twice — in palette.cpp and in nav_pickers.cpp — each
// commented "the SAME predicate" as the other. One definition makes that
// comment true rather than aspirational.
//
// Lives here rather than beside the command table because it needs the Model,
// and command_palette.hpp is deliberately Model-free.
[[nodiscard]] PaletteContext palette_context(const Model& m);

} // namespace agentty::ui
