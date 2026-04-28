#pragma once
#include <maya/widget/context_gauge.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::ContextGauge::Config context_gauge_config(const Model& m);

} // namespace moha::ui
