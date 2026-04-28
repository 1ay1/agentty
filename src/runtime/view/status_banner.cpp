#include "moha/runtime/view/status_banner.hpp"

#include <chrono>

namespace moha::ui {

maya::StatusBanner::Config status_banner_config(const Model& m) {
    maya::StatusBanner::Config cfg;
    auto now = std::chrono::steady_clock::now();

    // Treat expired toasts as absent — the reducer's ClearStatus
    // cleaner stops firing once Phase=Idle drops the Tick subscription,
    // so checking expiry here keeps the banner from flickering back
    // on a resize-driven repaint.
    bool has_status = !m.s.status.empty()
                      && m.s.status != "ready"
                      && m.s.status_active(now);
    if (!has_status) return cfg;       // empty text → blank slot

    cfg.text     = m.s.status;
    cfg.is_error = m.s.status.rfind("error:", 0) == 0;
    return cfg;
}

} // namespace moha::ui
