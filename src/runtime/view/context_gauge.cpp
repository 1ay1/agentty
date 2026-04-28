#include "moha/runtime/view/context_gauge.hpp"

namespace moha::ui {

maya::ContextGauge::Config context_gauge_config(const Model& m) {
    maya::ContextGauge::Config cfg;
    cfg.used     = m.s.tokens_in;
    cfg.max      = m.s.context_max;
    cfg.cells    = 10;
    cfg.show_bar = true;     // StatusBar overrides per-frame based on width
    return cfg;
}

} // namespace moha::ui
