#pragma once
#include <maya/widget/shortcut_row.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::ShortcutRow::Config shortcut_row_config(const Model& m);

} // namespace agentty::ui
