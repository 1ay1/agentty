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

// The pane's state. Named `AppearancePane` rather than `Appearance` because
// the panel SLOT type in slot.hpp is what the rest of the runtime says when
// it means "the Appearance overlay" — that one inherits this and adds the
// parent-snapshot mixin, and two types with one name in one namespace is a
// redefinition, not a convenience.
struct AppearancePane {
    form::Form form;

    // The theme picker, when open over this pane. A Pick row hands off to a
    // real overlay rather than growing the dropdown into a worse picker, and
    // this is its state: the query being typed and where the cursor sits.
    struct ThemePicker {
        std::string query;
        int         index  = 0;
        int         scroll = 0;
        // The theme in use when the browser opened. Moving the highlight
        // APPLIES a scheme (the list is its own preview), so Esc needs
        // somewhere to put back — without this, cancelling would leave you
        // wearing the last thing you merely looked at.
        std::string restore;
    };
    bool        picking = false;
    ThemePicker picker;
};

// Build the pane's rows from the prefs. Pure: prefs in, form out, so the
// view can rebuild it whenever the model changes and never hold stale rows.
[[nodiscard]] form::Form build_appearance_form(const ui_prefs::Prefs& p, bool tty);

// The schemes a query matches, most relevant first. Shared by the picker's
// view and its reducer so what is listed and what Enter selects cannot
// diverge — the same rule the form's Options follows.
[[nodiscard]] std::vector<std::string> matching_themes(std::string_view query);

}  // namespace agentty::ui::panel
