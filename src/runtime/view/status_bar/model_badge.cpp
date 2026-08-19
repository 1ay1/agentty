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
    const std::string& model = m.d.model_id.value;
    maya::ModelBadge mb{model};
    mb.set_compact(true);
    // Unknown family (new Claude line, local model, aggregator id): the badge
    // must never fall back to the raw wire id — give it the same prettified
    // label the picker and turn headers use.
    mb.set_fallback_label(pretty_model_label(model));

    // Provider suffix: a dim "· Anthropic" so multi-provider users always see
    // WHICH backend the model runs on, without stealing the model's colour.
    const std::string prov = provider::provider_display_name(provider::active());

    if (model.empty()) {
        // No model yet (e.g. an ACP agent that picks its own): show just the
        // provider so the slot is never blank.
        return h(text("● ", fg_dim(muted)),
                 text(prov, fg_dim(muted))).build();
    }
    return h(mb.build(),
             text(" · ", fg_dim(muted)),
             text(prov, fg_dim(muted))).build();
}

} // namespace agentty::ui
