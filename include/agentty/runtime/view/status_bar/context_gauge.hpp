#pragma once
#include <maya/widget/context_gauge.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::ContextGauge::Config context_gauge_config(const Model& m);

} // namespace agentty::ui
