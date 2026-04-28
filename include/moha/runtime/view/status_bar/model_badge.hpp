#pragma once
#include <maya/widget/model_badge.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// ModelBadge is a compact one — a colored ● + display name. Returns
// the configured widget (callers `.build()` it where they need an
// Element since ModelBadge predates the Config-pattern reshape).
[[nodiscard]] maya::ModelBadge model_badge_config(const Model& m);

} // namespace moha::ui
