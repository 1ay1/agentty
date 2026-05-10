#pragma once
#include <maya/widget/composer.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Composer::Config composer_config(const Model& m);

} // namespace agentty::ui
