#pragma once
#include <maya/maya.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element diff_review(const Model& m);

} // namespace agentty::ui
