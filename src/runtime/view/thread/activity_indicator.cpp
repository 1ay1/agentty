#include "agentty/runtime/view/thread/activity_indicator.hpp"

#include <algorithm>
#include <string>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

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

    const auto caps = ModelCapabilities::from_id(m.d.model_id.value);
    maya::Color edge = caps.is_opus()   ? accent
                     : caps.is_sonnet() ? info
                     : caps.is_haiku()  ? success
                                        : highlight;
    maya::ActivityIndicator::Config cfg;
    cfg.edge_color    = edge;
    cfg.spinner_glyph = std::string{m.s.spinner.current_frame()};
    cfg.label         = std::string{phase_verb(m.s.phase)};
    return cfg;
}

} // namespace agentty::ui
