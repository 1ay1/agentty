#pragma once
#include <maya/maya.hpp>
#include "moha/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::Element status_bar(const Model& m);

} // namespace moha::ui
