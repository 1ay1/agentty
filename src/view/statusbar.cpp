#include "moha/view/statusbar.hpp"

#include <maya/widget/activity_bar.hpp>

#include "moha/view/helpers.hpp"
#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {
Element shortcut_hint(const char* key, const char* label) {
    return h(
        text(key, fg_bold(fg)),
        text(label, fg_dim(muted))
    ).build();
}
Element shortcut_sep() { return text(" \u00B7 ", fg_dim(muted)); }
} // namespace

Element status_bar(const Model& m) {
    ActivityBar::Config cfg;
    cfg.separator_style = Style{}.with_fg(muted).with_dim();
    cfg.label_style     = Style{}.with_fg(muted);
    cfg.value_style     = Style{}.with_fg(fg);
    cfg.accent_style    = Style{}.with_fg(accent).with_bold();
    ActivityBar bar(cfg);

    bar.set_model(m.model_id.value);
    bar.set_tokens(m.stream.tokens_in, m.stream.tokens_out);

    if (m.stream.context_max > 0) {
        int pct = (m.stream.tokens_in + m.stream.tokens_out) * 100 / m.stream.context_max;
        bar.set_context_percent(pct);
    }

    bar.add_section(
        std::string{phase_glyph(m.stream.phase)},
        std::string{phase_verb(m.stream.phase)},
        Style{}.with_fg(phase_color(m.stream.phase)).with_bold());

    bar.add_section(
        "\u25CF",
        std::string{profile_label(m.profile)},
        Style{}.with_fg(profile_color(m.profile)));

    // Surface the most recent transient message (error or status hint)
    // — invisible state strands the user when something fails silently.
    if (!m.stream.status.empty() && m.stream.status != "ready") {
        bool is_err = m.stream.status.rfind("error:", 0) == 0;
        bar.add_section(
            is_err ? "\u26A0" : "",
            m.stream.status,
            Style{}.with_fg(is_err ? danger : muted));
    }

    // Just the essentials — the command palette (^K) exposes the rest, and
    // a packed hint row is harder to scan than a sparse one.
    auto shortcuts = h(
        text(" ", fg_dim(muted)),
        shortcut_hint("^K",    " palette"),  shortcut_sep(),
        shortcut_hint("^J",    " threads"),  shortcut_sep(),
        shortcut_hint("S-Tab", " profile"),  shortcut_sep(),
        shortcut_hint("^/",    " models"),   shortcut_sep(),
        shortcut_hint("^N",    " new"),      shortcut_sep(),
        shortcut_hint("^C",    " quit")
    );

    return v(bar.build(), shortcuts.build()).build();
}

} // namespace moha::ui
