#include "agentty/runtime/view/status_bar/model_badge.hpp"

#include <maya/widget/model_badge.hpp>

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/view/helpers.hpp"  // pretty_model_label
#include "agentty/runtime/view/palette.hpp"   // fg_dim / muted palette

namespace agentty::ui {

maya::Element model_badge_config(const Model& m) {
    using namespace maya;
    using namespace maya::dsl;

    // Model chip: maya's ModelBadge parses the id into a rich family+version
    // ("● Sonnet") coloured by family. The model was previously invisible while
    // idle — it only appeared in assistant turn headers — so a freshly-switched
    // model had no on-screen home. Now it lives here.
    //
    // While a Smart Mode turn is in flight the badge names the model ACTUALLY
    // serving it (m.s.smart_turn_model), not the picker selection: under
    // orchestration those differ, and showing the selection meant the chip
    // claimed "Mistral" while every token came from the Strategic model. The
    // selection reappears the moment the turn settles.
    const std::string& model = !m.s.smart_turn_model.empty()
                                   ? m.s.smart_turn_model
                                   : m.d.model_id.value;
    maya::ModelBadge mb{model};
    mb.set_compact(true);
    // Unknown family (new Claude line, local model, aggregator id): the badge
    // must never fall back to the raw wire id — give it the same prettified
    // label the picker and turn headers use.
    mb.set_fallback_label(pretty_model_label(model));

    // Provider suffix: a dim "· Anthropic" so multi-provider users always see
    // WHICH backend the model runs on, without stealing the model's colour.
    const std::string prov = provider::provider_display_name(provider::active());

    // Reasoning-effort chip: when a tier is active AND the model can reason,
    // ride a compact "· ◇high" so the current effort is visible at a glance
    // without opening the picker — the same tier you set there (←/→). Uses
    // resolved_caps so it never shows on a model that can't take effort (or
    // where a stale pick would be dropped at send time).
    Element effort_chip = text("");
    bool has_effort_chip = false;
    if (m.d.effort != Effort::None && !model.empty()) {
        const auto caps = resolved_caps(model);
        if (effort_capable(caps)) {
            effort_chip = h(text(" \xc2\xb7 ", fg_dim(muted)),
                            text("\xe2\x97\x87" +
                                 std::string{effort_label(m.d.effort)},
                                 fg_dim(muted))).build();
            has_effort_chip = true;
        }
    }

    if (model.empty()) {
        // No model yet (e.g. an ACP agent that picks its own): show just the
        // provider so the slot is never blank.
        return h(text("\xe2\x97\x8f ", fg_dim(muted)),
                 text(prov, fg_dim(muted))).build();
    }
    // Update chip: when a newer release is known (background check), a
    // compact "⬆ vX.Y.Z" rides beside the model badge — bright enough to
    // notice, quiet enough to ignore. The palette's "Update agentty" (and
    // `agentty update`) are the actions; this chip is only the signal.
    if (!m.s.update_latest.empty()) {
        return h(mb.build(),
                 text(" \xc2\xb7 ", fg_dim(muted)),
                 text(prov, fg_dim(muted)),
                 has_effort_chip ? effort_chip : text(""),
                 text("  \xe2\xac\x86 v" + m.s.update_latest,
                      fg_of(maya::Color::green()))).build();
    }
    return h(mb.build(),
             text(" \xc2\xb7 ", fg_dim(muted)),
             text(prov, fg_dim(muted)),
             has_effort_chip ? effort_chip : text("")).build();
}

} // namespace agentty::ui
