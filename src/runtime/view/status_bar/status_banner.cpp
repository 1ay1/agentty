#include "agentty/runtime/view/status_bar/status_banner.hpp"

#include <chrono>

namespace agentty::ui {

// Toast notifications ride on the StatusBanner row, which maya::StatusBar
// now uses to take over the activity_row slot entirely (full-width, bright
// foreground on accent background) whenever `text` is non-empty. The
// shortcut row was retired; shortcuts moved to the welcome screen.
//
// We surface `m.s.status` here as long as it's both non-empty AND still
// inside its TTL window (`status_active`). "ready" is the calm/idle
// pseudo-status and shouldn't strobe the toast band, so it's filtered
// out. `is_error` keys off the conventional `error:` prefix so the
// widget picks the red-tint variant; everything else (retry, cancel,
// compact, "context compacted", …) renders on the phase color.
maya::StatusBanner::Config status_banner_config(const Model& m) {
    maya::StatusBanner::Config cfg;
    if (m.s.status.empty() || m.s.status == "ready") return cfg;
    if (!m.s.status_active(std::chrono::steady_clock::now())) return cfg;
    cfg.text     = m.s.status;
    cfg.is_error = m.s.status.rfind("error:", 0) == 0;
    return cfg;
}

} // namespace agentty::ui
