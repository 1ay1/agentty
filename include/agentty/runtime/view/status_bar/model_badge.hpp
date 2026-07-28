#pragma once
#include <maya/element/element.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// The status-bar identity chip: a colored model chip ("● Sonnet") from maya's
// ModelBadge, plus a dim provider suffix ("· Anthropic") so BOTH the active
// model and the active backend are visible at a glance — the model was
// previously invisible while idle, and the provider needs a persistent home
// for multi-provider users. Returns a built Element (composed here rather than
// via the single-string widget so it can carry both facts).
[[nodiscard]] maya::Element model_badge_config(const Model& m);

} // namespace agentty::ui
