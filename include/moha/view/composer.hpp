#pragma once
#include <maya/maya.hpp>
#include "moha/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element composer(const Model& m);

} // namespace moha::ui
