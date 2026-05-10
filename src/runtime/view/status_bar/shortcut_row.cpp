#include "agentty/runtime/view/status_bar/shortcut_row.hpp"

#include <chrono>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::ShortcutRow::Config shortcut_row_config(const Model& m) {
    maya::ShortcutRow::Config cfg;
    cfg.label_min_width = 110;
    cfg.full_min_width  = 55;
    cfg.text_color      = fg;

    // Notification takeover: when there's an active status message, the
    // shortcut row doubles as the notification slot — keybindings are
    // displaced by a single banner-style "binding" carrying the message.
    // Cleared the moment the toast expires (status_active() returns false)
    // so the row reverts to bindings without a manual transition.
    //
    // Why hijack ShortcutRow instead of the dedicated StatusBanner row:
    // the bottom of the status bar already owns one row of vertical real
    // estate for shortcuts; surfacing notifications there avoids a second
    // row of jitter and keeps the panel's height steady. The standalone
    // StatusBanner is left blank by status_banner_config — a 1-cell
    // placeholder that maya's StatusBar uses as a separator strip.
    auto now = std::chrono::steady_clock::now();
    const bool has_status = !m.s.status.empty()
                            && m.s.status != "ready"
                            && m.s.status_active(now);
    if (has_status) {
        const bool is_error = m.s.status.rfind("error:", 0) == 0;
        // ShortcutRow renders `key (bold) label (dim)`. We slot the
        // banner glyph into `key` (bold-colored) and the message text
        // into `label` (dim) — visually aligns with the StatusBanner
        // treatment (▎⚠ <text>) without needing a custom widget.
        // Priority is high and full_min_width is collapsed so the
        // notification is never dropped on narrow terminals — losing a
        // notification because the window is small would be the worst
        // failure mode of this widget.
        //
        // Queue-depth is intentionally NOT shown here: the composer's
        // own right-hand strip already renders "❚ N queued" whenever
        // any messages are queued, and surfacing the same count in two
        // places at once read as duplication. Composer is the source
        // of truth for queue depth; the shortcut row owns transient
        // status / retry / cancel toasts.
        cfg.bindings = {
            {.key       = is_error ? "\xe2\x96\x8e\xe2\x9a\xa0"   // ▎⚠
                                   : "\xe2\x96\x8e",              // ▎
             .label     = m.s.status,
             .key_color = is_error ? danger : maya::Color::bright_black(),
             .priority  = 1000},
        };
        cfg.full_min_width = 0;     // never drop the notification
        return cfg;
    }

    cfg.bindings = {
        {.key="^K",    .label="palette", .key_color=highlight, .priority=10},
        {.key="^J",    .label="threads", .key_color=highlight, .priority=10},
        {.key="^T",    .label="todo",    .key_color=highlight, .priority=10},
        {.key="S-Tab", .label="profile", .key_color=highlight, .priority=4},
        {.key="^/",    .label="models",  .key_color=highlight, .priority=4},
        {.key="^N",    .label="new",     .key_color=success,   .priority=10},
        {.key="^C",    .label="quit",    .key_color=danger,    .priority=10},
    };
    return cfg;
}

} // namespace agentty::ui
