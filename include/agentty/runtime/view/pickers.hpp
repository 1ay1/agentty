#pragma once
#include <maya/maya.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element model_picker(const Model& m);
[[nodiscard]] maya::Element thread_list(const Model& m);
[[nodiscard]] maya::Element command_palette(const Model& m);
[[nodiscard]] maya::Element mention_palette(const Model& m);
[[nodiscard]] maya::Element symbol_palette(const Model& m);
[[nodiscard]] maya::Element todo_modal(const Model& m);

} // namespace agentty::ui
