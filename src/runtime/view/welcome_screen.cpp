#include "moha/runtime/view/welcome_screen.hpp"

#include <maya/widget/model_badge.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

maya::WelcomeScreen::Config welcome_screen_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);

    maya::WelcomeScreen::Config cfg;
    cfg.wordmark       = {"\xe2\x94\x8c\xe2\x94\xac\xe2\x94\x90\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90\xe2\x94\xac \xe2\x94\xac\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90",
                          "\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82\xe2\x94\x82 \xe2\x94\x82\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4\xe2\x94\x9c\xe2\x94\x80\xe2\x94\xa4",
                          "\xe2\x94\xb4 \xe2\x94\xb4\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98\xe2\x94\xb4 \xe2\x94\xb4\xe2\x94\xb4 \xe2\x94\xb4"};
    cfg.wordmark_color = accent;
    cfg.tagline        = "a calm middleware between you and the model";
    cfg.model_badge    = mb.build();
    cfg.profile_label  = std::string{profile_label(m.d.profile)};
    cfg.profile_color  = profile_color(m.d.profile);
    cfg.starters_title = "Try";
    cfg.starters       = {"Implement a small feature",
                          "Refactor or clean up this file",
                          "Explain what this code does",
                          "Write tests for ..."};
    cfg.hint_intro     = "type to begin";
    cfg.hints          = {{"^K", " palette", highlight},
                          {"^J", " threads", highlight},
                          {"^N", " new",     success}};
    cfg.accent_color   = accent;
    cfg.text_color     = fg;
    return cfg;
}

} // namespace moha::ui
