#include "moha/runtime/view/composer.hpp"

#include <maya/widget/composer.hpp>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;

namespace {

// Map moha runtime state → widget State enum. The widget owns all
// downstream visual decisions (border color, prompt boldness, placeholder
// text, height pin) — this is pure data translation.
maya::Composer::State composer_state(const Model& m) {
    if (m.s.is_awaiting_permission()) return maya::Composer::State::AwaitingPermission;
    if (m.s.is_executing_tool())      return maya::Composer::State::ExecutingTool;
    if (m.s.is_streaming())           return maya::Composer::State::Streaming;
    return maya::Composer::State::Idle;
}

} // namespace

Element composer(const Model& m) {
    return maya::Composer{{
        .text          = m.ui.composer.text,
        .cursor        = m.ui.composer.cursor,
        .state         = composer_state(m),
        .active_color  = phase_color(m.s.phase),
        .text_color    = fg,
        .accent_color  = accent,
        .warn_color    = warn,
        .highlight_color = highlight,
        .queued        = m.ui.composer.queued.size(),
        .profile       = {.label = std::string{profile_label(m.d.profile)},
                          .color = profile_color(m.d.profile)},
        .expanded      = m.ui.composer.expanded,
    }}.build();
}

} // namespace moha::ui
