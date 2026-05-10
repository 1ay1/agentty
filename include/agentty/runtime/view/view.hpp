#pragma once
// agentty::ui::view — top-level composition.
//
// Assembles the per-panel views into the root Element.  All overlay logic
// (modals, pickers) lives here so per-panel files stay focused.

#include <maya/maya.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element view(const Model& m);

} // namespace agentty::ui
