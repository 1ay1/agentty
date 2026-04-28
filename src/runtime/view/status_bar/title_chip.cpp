#include "moha/runtime/view/status_bar/title_chip.hpp"

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

maya::TitleChip::Config title_chip_config(const Model& m) {
    maya::TitleChip::Config cfg;
    cfg.title      = m.d.current.title;
    cfg.edge_color = phase_color(m.s.phase);
    cfg.text_color = fg;
    cfg.max_chars  = 28;
    return cfg;
}

} // namespace moha::ui
