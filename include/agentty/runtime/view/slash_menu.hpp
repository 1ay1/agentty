#pragma once
// The composer's `/` menu: the rows slash::menu_rows picked, drawn right
// above the composer box. Empty element when the menu is hidden.
#include <maya/element/element.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element slash_menu(const Model& m);

// Rows the menu would show (0 when hidden). The composer's hint row reads
// it to switch to the menu's keys.
[[nodiscard]] bool slash_menu_open(const Model& m);

}  // namespace agentty::ui
