#pragma once
#include <maya/widget/model_badge.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// ModelBadge is a compact one — a colored ● + display name. Returns
// the configured widget (callers `.build()` it where they need an
// Element since ModelBadge predates the Config-pattern reshape).
[[nodiscard]] maya::ModelBadge model_badge_config(const Model& m);

} // namespace agentty::ui
