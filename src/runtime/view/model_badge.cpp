#include "moha/runtime/view/model_badge.hpp"

namespace moha::ui {

maya::ModelBadge model_badge_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);
    return mb;
}

} // namespace moha::ui
