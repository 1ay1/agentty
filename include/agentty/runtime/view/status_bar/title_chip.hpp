#pragma once
#include <maya/widget/title_chip.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::TitleChip::Config title_chip_config(const Model& m);

} // namespace agentty::ui
