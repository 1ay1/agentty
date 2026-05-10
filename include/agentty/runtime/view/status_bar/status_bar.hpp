#pragma once
#include <maya/widget/status_bar.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::StatusBar::Config status_bar_config(const Model& m);

} // namespace agentty::ui
