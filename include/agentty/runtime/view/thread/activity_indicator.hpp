#pragma once
#include <optional>
#include <string_view>
#include <vector>
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

// Shared host-owned rotating word pool for the ActivityIndicator.
// Returned by const-ref; lifetime is static. Both the bottom-of-
// thread indicator and the in-Turn placeholder indicator wire this
// into ActivityIndicator::Config::words so the widget stays
// content-agnostic.
[[nodiscard]] const std::vector<std::string_view>& activity_indicator_words();

} // namespace agentty::ui
