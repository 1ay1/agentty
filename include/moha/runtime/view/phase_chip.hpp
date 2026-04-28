#pragma once
#include <maya/widget/phase_chip.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::PhaseChip::Config phase_chip_config(const Model& m);

} // namespace moha::ui
