#include "agentty/runtime/view/thread/welcome_screen.hpp"

#include <maya/widget/model_badge.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::WelcomeScreen::Config welcome_screen_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);

    maya::WelcomeScreen::Config cfg;
    cfg.wordmark       = {"\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\x8c\xe2\x94\x90 \xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90\xe2\x94\xac \xe2\x94\xac",
                          "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4\xe2\x94\x82 \xe2\x94\x90\xe2\x94\x9c\xe2\x94\x80 \xe2\x94\x82\xe2\x94\x94\xe2\x94\x90 \xe2\x94\x82  \xe2\x94\x82 \xe2\x94\x94\xe2\x94\xac\xe2\x94\x98",
                          "\xe2\x94\xb4 \xe2\x94\xb4\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98\xe2\x94\x98 \xe2\x94\x98 \xe2\x94\xb4  \xe2\x94\xb4  \xe2\x94\xb4 "};
    cfg.wordmark_color = accent;
    cfg.tagline        = "a calm middleware between you and the model";
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
    cfg.hints          = {{"^K", " palette", highlight},
                          {"^J", " threads", highlight},
                          {"^N", " new",     success}};
    cfg.accent_color   = accent;
    cfg.text_color     = fg;
    return cfg;
}

} // namespace agentty::ui
