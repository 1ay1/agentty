#pragma once
#include <maya/widget/status_banner.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::StatusBanner::Config status_banner_config(const Model& m);

} // namespace agentty::ui
