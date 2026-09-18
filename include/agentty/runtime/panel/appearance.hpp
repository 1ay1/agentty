#pragma once
// agentty::ui::panel::Appearance — the look-and-feel pane.
//
// A form pane like Retrieval and Smart Mode: the pane holds a form::Form and
// nothing else, so navigation, dropdowns and editing are the shared reducer's
// job and maya::Form owns every glyph. What lives here is the one genuinely
// pane-specific thing — which row ids exist, so the reducer can read values
// back out by name rather than by position.
//
// ── Why the theme row is a Pick and the rest are Choices ─────────────────
//
// form.hpp draws the line precisely: a Choice is a SMALL CLOSED enum you can
// name in a comment, a Pick is a large or dynamic set that needs searching.
// Colors, Background, Density, Motion are enums of three to five values, so
// they cycle with ←/→ and open a short dropdown on Enter. Themes are 57 and
// grow whenever the generator is re-run, so they are a Pick that hands off to
// a real searchable picker — the same rule Smart Mode follows for models.

#include <string>
#include <vector>
#include <string_view>

#include "agentty/domain/ui_prefs.hpp"
#include "agentty/runtime/panel/form.hpp"

#include "agentty/runtime/panel/filtered_picker.hpp"

namespace agentty::ui::panel {

// Row ids. Named constants rather than literals at both the build site and
// the read site: a typo in one of two matching strings is a silently dead
// setting, and the compiler cannot see it.
inline constexpr std::string_view kApTheme      = "theme";
inline constexpr std::string_view kApTier       = "tier";
inline constexpr std::string_view kApPolarity   = "polarity";
inline constexpr std::string_view kApDensity    = "density";
inline constexpr std::string_view kApProseWidth = "prose_width";
inline constexpr std::string_view kApCompact    = "compact_turns";
inline constexpr std::string_view kApMotion     = "motion";
inline constexpr std::string_view kApSyntax     = "syntax";
inline constexpr std::string_view kApToolOutput = "tool_output";
inline constexpr std::string_view kApThinking   = "thinking";
inline constexpr std::string_view kApTimestamps = "timestamps";

// ── What feeds the theme picker ─────────────────────────────────────────
//
// The scheme list is a process-lifetime constant (maya's built-in table), so
// the "snapshot" is always ready and never refills. It still goes through
// SnapshotSource because that is the seam FilteredPicker reads through, and
// a picker with one uniform shape is the point of this file.
[[nodiscard]] ui::SnapshotSource<std::string> theme_source();

// Fuzzy subsequence match over scheme names, best first — the same ranking
// `matching_themes` applied, now expressed as the picker's filter so the
// list and the cursor cannot disagree about what "row 4" means.
[[nodiscard]] ui::FilterFn<std::string> theme_filter();

// The pane's state. Named `AppearancePane` rather than `Appearance` because
// the panel SLOT type in slot.hpp is what the rest of the runtime says when
// it means "the Appearance overlay" — that one inherits this and adds the
// parent-snapshot mixin, and two types with one name in one namespace is a
// redefinition, not a convenience.
struct AppearancePane {
    form::Form form;

    // The theme picker, when open over this pane. A Pick row hands off to a
    // real overlay rather than growing the dropdown into a worse picker.
    //
    // This is a FilteredPicker, not a hand-rolled {query, index, scroll}
    // bundle, and that is the whole point. The bundle is what broke: the
    // reducer advanced `index` while `scroll` sat unread beside it, so the
    // highlight walked off the bottom of 615 schemes and Down appeared to do
    // nothing. The picker owns query, cursor AND the derived viewport
    // together (see visible()/visible_entries()), so there is no second
    // field to leave behind.
    struct ThemePicker {
        ThemePicker() : picker(theme_source(), theme_filter()) {}

        ui::FilteredPicker<std::string> picker;

        // The theme in use when the browser opened. Moving the highlight
        // APPLIES a scheme (the list is its own preview), so Esc needs
        // somewhere to put back — without this, cancelling would leave you
        // wearing the last thing you merely looked at. Never drawn, so it is
        // exempt from the frame hash.
        std::string restore;
    };
    bool        picking = false;
    ThemePicker picker;
};

// The picker owns every visible axis (query + cursor); `restore` is the Esc
// undo stash and never reaches the screen.
inline auto visual_parts(const AppearancePane::ThemePicker& p) {
    return std::make_tuple(visual::ref(p.picker), visual::exempt);
}

}  // namespace agentty::ui::panel

namespace agentty::visual {
// The reviewed claim. ThemePicker has a user-provided constructor (it seeds
// the FilteredPicker with its source + filter), so the brace-arity probe
// reads 0 and the structural proof cannot check this one itself. Both members
// are accounted for above: `picker` is the visible state, `restore` is the
// Esc undo stash that is never drawn.
template <>
inline constexpr bool
    trusted_parts<ui::panel::AppearancePane::ThemePicker> = true;
}  // namespace agentty::visual

namespace agentty::ui::panel {
static_assert(visual::parts_cover_all<AppearancePane::ThemePicker>);

// Build the pane's rows from the prefs. Pure: prefs in, form out, so the
// view can rebuild it whenever the model changes and never hold stale rows.
[[nodiscard]] form::Form build_appearance_form(const ui_prefs::Prefs& p, bool tty);

// The schemes a query matches, most relevant first. Shared by the picker's
// view and its reducer so what is listed and what Enter selects cannot
// diverge — the same rule the form's Options follows.
//
// Returns a reference into a process-lifetime memo: a single arrow key asks
// this question three times (wrap the index, name the landing row, draw), and
// returning by value made each of those rebuild 615 strings.
[[nodiscard]] const std::vector<std::string>& matching_themes(std::string_view query);

}  // namespace agentty::ui::panel
