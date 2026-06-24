#include "agentty/runtime/view/status_bar/model_badge.hpp"

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"

namespace agentty::ui {

maya::ModelBadge model_badge_config(const Model& m) {
    // Status bar shows the active PROVIDER ("Anthropic", "Groq",
    // "Ollama", …), not the model — the model name already lives in each
    // assistant turn header. This tells the user which backend they're
    // talking to at a glance, independent of the model id.
    (void)m;
    maya::ModelBadge mb{provider::provider_display_name(provider::active())};
    mb.set_compact(true);
    return mb;
}

} // namespace agentty::ui
