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
} // namespace

Element status_bar(const Model& m) {
    ActivityBar bar;
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
        "",
        "⟨ " + std::string{profile_label(m.profile)} + " ⟩",
        Style{}.with_fg(profile_color(m.profile)));

    // Surface the most recent transient message (error or status hint)
    // — invisible state strands the user when something fails silently.
    if (!m.stream.status.empty() && m.stream.status != "ready") {
        bool is_err = m.stream.status.rfind("error:", 0) == 0;
        bar.add_section(
            is_err ? "⚠" : "",
            m.stream.status,
            Style{}.with_fg(is_err ? danger : muted));
    }

    auto sep = text(" · ", fg_dim(muted));

    auto shortcuts = h(
        text(" ", fg_dim(muted)),
        shortcut_hint("^/", " model"),
        sep,
        shortcut_hint("S-Tab", " profile"),
        sep,
        shortcut_hint("^J", " threads"),
        sep,
        shortcut_hint("^K", " palette"),
        text("   ", fg_dim(muted)),
        shortcut_hint("^T", " plan"),
        sep,
        shortcut_hint("^R", " review"),
        sep,
        shortcut_hint("^N", " new"),
        text("   ", fg_dim(muted)),
        shortcut_hint("^C", " quit")
    );

    return v(bar.build(), shortcuts.build()).build();
}

} // namespace moha::ui
