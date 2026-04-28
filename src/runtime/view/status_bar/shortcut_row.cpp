#include "moha/runtime/view/status_bar/shortcut_row.hpp"

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

maya::ShortcutRow::Config shortcut_row_config(const Model& /*m*/) {
    maya::ShortcutRow::Config cfg;
    cfg.bindings = {
        {.key="^K",    .label="palette", .key_color=highlight, .priority=10},
        {.key="^J",    .label="threads", .key_color=highlight, .priority=10},
        {.key="^T",    .label="todo",    .key_color=highlight, .priority=10},
        {.key="S-Tab", .label="profile", .key_color=highlight, .priority=4},
        {.key="^/",    .label="models",  .key_color=highlight, .priority=4},
        {.key="^N",    .label="new",     .key_color=success,   .priority=10},
        {.key="^C",    .label="quit",    .key_color=danger,    .priority=10},
    };
    cfg.label_min_width = 110;
    cfg.full_min_width  = 55;
    cfg.text_color      = fg;
    return cfg;
}

} // namespace moha::ui
