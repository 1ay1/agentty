#include "agentty/runtime/view/status_bar/title_chip.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::TitleChip::Config title_chip_config(const Model& m) {
    maya::TitleChip::Config cfg;
    cfg.title      = m.d.current.title;
    cfg.edge_color = phase_color(m.s.phase);
    cfg.text_color = fg;
    cfg.max_chars  = 28;
    return cfg;
}

} // namespace agentty::ui
