#include "agentty/runtime/view/status_bar/status_banner.hpp"

namespace agentty::ui {

// Notifications now ride on the ShortcutRow (see shortcut_row.cpp): the
// shortcut row swaps its keybindings for a banner-style notification
// when m.s.status is active and reverts when the toast expires. The
// StatusBanner row is left blank — maya's StatusBar still draws a
// 1-cell empty strip there, which acts as a separator between the
// activity row and the bottom shortcut row. Eliminating it would
// require a maya-side change to StatusBar's row list; keeping it
// blank here is non-invasive and preserves the existing panel height.
maya::StatusBanner::Config status_banner_config(const Model& /*m*/) {
    return {};
}

} // namespace agentty::ui
