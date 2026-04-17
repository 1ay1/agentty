#pragma once
#include <maya/maya.hpp>
#include "moha/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element changes_strip(const Model& m);

} // namespace moha::ui
