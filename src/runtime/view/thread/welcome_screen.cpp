#include "agentty/runtime/view/thread/welcome_screen.hpp"

#include <maya/widget/model_badge.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::WelcomeScreen::Config welcome_screen_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);

    maya::WelcomeScreen::Config cfg;
    cfg.sigil_color = role_brand_alt;                   // bright_magenta flagship sigil
    cfg.tagline     = "a calm middleware between you and the model";
    cfg.model_badge    = mb.build();
    cfg.profile_label  = std::string{profile_label(m.d.profile)};
    cfg.profile_color  = profile_color(m.d.profile);
    // Starters panel intentionally empty — maya skips the whole "Try
    // • …" card (and its bracketing blank rows) when the list has no
    // entries, so the welcome screen flows straight from the
    // model/profile chip row into the bottom hint. The placeholder
    // suggestions ("Implement a small feature", etc.) read as
    // landing-page filler in a TUI; the wordmark + tagline + bottom
    // hint already make the affordance ("type to begin") clear.
    cfg.starters_title = {};
    cfg.starters       = {};
    cfg.hint_intro     = "type to begin";
    // Full keybinding map (was previously in the bottom status bar's
    // ShortcutRow, which has been retired — the status row now serves
    // as a high-visibility toast slot instead). Per-key colors signal
    // what each binding does at a glance: pickers (palette/threads/
    // todo/models) share their action-surface hue, profile/new/quit
    // get distinct semantic colors.
    cfg.hints          = {{"^K",    " palette", code_path},      // cyan — actions
                          {"^J",    " threads", role_info},      // blue — navigation
                          {"^T",    " todo",    warn},           // yellow — planning
                          {"S-Tab", " profile", role_brand},     // magenta — identity
                          {"^/",    " models",  role_brand_alt}, // bright magenta
                          {"^N",    " new",     success},        // green — create
                          {"^C",    " quit",    danger}};        // red — destructive
    cfg.accent_color   = role_brand;
    cfg.text_color     = fg;
    return cfg;
}

} // namespace agentty::ui
