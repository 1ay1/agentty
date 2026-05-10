#pragma once
#include <optional>
#include <maya/widget/activity_indicator.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Pick the bottom-of-thread "still working…" indicator config, if any.
// Suppressed when the active assistant turn already shows a Timeline
// spinner — its in-progress card + the status bar's spinner already
// carry the "still working" signal; a second one stacked under it
// was just duplicate chrome.
[[nodiscard]] std::optional<maya::ActivityIndicator::Config>
    activity_indicator_config(const Model& m);

} // namespace agentty::ui
