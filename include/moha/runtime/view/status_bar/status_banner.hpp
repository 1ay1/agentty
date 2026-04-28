#pragma once
#include <maya/widget/status_banner.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::StatusBanner::Config status_banner_config(const Model& m);

} // namespace moha::ui
