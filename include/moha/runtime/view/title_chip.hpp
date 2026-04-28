#pragma once
#include <maya/widget/title_chip.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::TitleChip::Config title_chip_config(const Model& m);

} // namespace moha::ui
