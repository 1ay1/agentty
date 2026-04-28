#pragma once
#include <maya/widget/shortcut_row.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

[[nodiscard]] maya::ShortcutRow::Config shortcut_row_config(const Model& m);

} // namespace moha::ui
