#include "moha/runtime/view/status_bar/status_bar.hpp"

#include "moha/runtime/view/status_bar/context_gauge.hpp"
#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/status_bar/model_badge.hpp"
#include "moha/runtime/view/status_bar/phase_chip.hpp"
#include "moha/runtime/view/status_bar/shortcut_row.hpp"
#include "moha/runtime/view/status_bar/status_banner.hpp"
#include "moha/runtime/view/status_bar/title_chip.hpp"
#include "moha/runtime/view/status_bar/token_stream_sparkline.hpp"

namespace moha::ui {

maya::StatusBar::Config status_bar_config(const Model& m) {
    const bool is_streaming = m.s.is_streaming() && m.s.active();

    maya::StatusBar::Config cfg;
    cfg.phase_color   = phase_color(m.s.phase);
    cfg.breadcrumb    = title_chip_config(m);
    cfg.phase         = phase_chip_config(m);
    cfg.token_stream  = token_stream_sparkline_config(m);
    cfg.model_badge   = model_badge_config(m).build();
    cfg.context       = context_gauge_config(m);
    cfg.status_banner = status_banner_config(m);
    cfg.shortcuts     = shortcut_row_config(m);

    // Streaming pushes the breadcrumb threshold up so the live
    // sparkline + tok/s readout has room to breathe without elbowing
    // the title.
    cfg.breadcrumb_min_width   = is_streaming ? 160 : 130;
    cfg.token_stream_min_width = 110;
    cfg.ctx_bar_min_width      = 55;
    return cfg;
}

} // namespace moha::ui
