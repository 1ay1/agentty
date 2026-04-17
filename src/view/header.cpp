#include "moha/view/header.hpp"

#include <format>

#include <maya/widget/model_badge.hpp>

#include "moha/view/helpers.hpp"
#include "moha/view/palette.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element header(const Model& m) {
    ModelBadge badge(m.model_id.value);
    auto prof_text = text(std::format(" {} ", profile_label(m.profile)),
                          fg_bold(profile_color(m.profile)).with_dim());
    return h(
        text(" moha ", fg_bold(accent)),
        text(" "),
        badge.build(),
        text(" "),
        prof_text,
        text("  "),
        text(phase_label(m.stream.phase), fg_of(muted)),
        spacer(),
        text(std::format("{}/{} tok", m.stream.tokens_in + m.stream.tokens_out, m.stream.context_max),
             fg_of(muted))
    ).build();
}

} // namespace moha::ui
