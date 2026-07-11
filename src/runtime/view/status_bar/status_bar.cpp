#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include "agentty/runtime/view/status_bar/context_gauge.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/status_bar/model_badge.hpp"
#include "agentty/runtime/view/status_bar/phase_chip.hpp"
#include "agentty/runtime/view/status_bar/status_banner.hpp"
#include "agentty/runtime/view/status_bar/title_chip.hpp"
#include "agentty/runtime/view/status_bar/token_stream_sparkline.hpp"

namespace agentty::ui {

maya::StatusBar::Config status_bar_config(const Model& m) {
    maya::StatusBar::Config cfg;
    cfg.phase_color   = phase_color(m.s.phase);
    cfg.breadcrumb    = title_chip_config(m);
    cfg.phase         = phase_chip_config(m);
    cfg.token_stream  = token_stream_sparkline_config(m);
    cfg.model_badge   = model_badge_config(m).build();
    cfg.context       = context_gauge_config(m);
    cfg.status_banner = status_banner_config(m);
    // Shortcuts row retired — the welcome screen carries the full
    // keybinding map. While a thread is active the user has already
    // internalised the bindings, and the status bar's middle row
    // doubles as the toast slot for transient notifications (retry,
    // cancel, compact, error) which is more useful real estate.

    // Width thresholds retired (maya 6263c4f): StatusBar's activity row
    // is now a measured degradation ladder — every fragment is built at
    // its real styled width and the row sheds detail until it fits. No
    // per-host knobs to tune.
    return cfg;
}

} // namespace agentty::ui
