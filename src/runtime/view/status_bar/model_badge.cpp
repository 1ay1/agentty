#include "agentty/runtime/view/status_bar/model_badge.hpp"

namespace agentty::ui {

maya::ModelBadge model_badge_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);
    return mb;
}

} // namespace agentty::ui
