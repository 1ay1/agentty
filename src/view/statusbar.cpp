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

    int pct = m.stream.context_max > 0
                ? (m.stream.tokens_in + m.stream.tokens_out) * 100 / m.stream.context_max
                : 0;
    bar.set_context_percent(pct);
    bar.set_status(std::string{phase_label(m.stream.phase)});
    bar.add_section("", std::string{profile_label(m.profile)},
        Style{}.with_fg(profile_color(m.profile)));

    auto shortcuts = h(
        text(" ", fg_dim(muted)),
        shortcut_hint("^/", " model  "),
        shortcut_hint("S-Tab", " profile  "),
        shortcut_hint("^J", " threads  "),
        shortcut_hint("^K", " palette  "),
        shortcut_hint("^R", " review  "),
        shortcut_hint("^N", " new  "),
        shortcut_hint("^C", " quit")
    );

    return v(bar.build(), shortcuts.build()).build();
}

} // namespace moha::ui
