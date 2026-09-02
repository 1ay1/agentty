#pragma once
#include <maya/element/element.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// The identity chip: a colored model chip ("● Sonnet") from maya's
// ModelBadge, plus a dim provider suffix ("· Anthropic") so BOTH the active
// model and the active backend are visible at a glance. It lives in the
// composer's footer row rather than the status bar: the status bar is the
// TURN's surface (phase, throughput, context) and churns while streaming,
// whereas the model is a property of what you are about to SEND — so it
// belongs next to the thing you type into, where it also stops competing
// with the phase chip for the shed budget. Returns a built Element (composed
// here rather than via the single-string widget so it can carry both facts).
[[nodiscard]] maya::Element model_badge_config(const Model& m);

} // namespace agentty::ui
