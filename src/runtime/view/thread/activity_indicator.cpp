#include "moha/runtime/view/thread/activity_indicator.hpp"

#include <algorithm>
#include <string>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

std::optional<maya::ActivityIndicator::Config>
activity_indicator_config(const Model& m) {
    if (!m.s.active()) return std::nullopt;
    if (m.d.current.messages.empty()) return std::nullopt;
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return std::nullopt;
    bool tl_visible =
        !last.tool_calls.empty()
        && std::any_of(last.tool_calls.begin(), last.tool_calls.end(),
                       [](const auto& tc){ return !tc.is_terminal(); });
    if (tl_visible) return std::nullopt;

    const auto& mid = m.d.model_id.value;
    maya::Color edge = (mid.find("opus")   != std::string::npos) ? accent
                     : (mid.find("sonnet") != std::string::npos) ? info
                     : (mid.find("haiku")  != std::string::npos) ? success
                                                                 : highlight;
    maya::ActivityIndicator::Config cfg;
    cfg.edge_color    = edge;
    cfg.spinner_glyph = std::string{m.s.spinner.current_frame()};
    cfg.label         = std::string{phase_verb(m.s.phase)};
    return cfg;
}

} // namespace moha::ui
