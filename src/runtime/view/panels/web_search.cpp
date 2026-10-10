// web_search.cpp — the Settings → Web Search panel, via the shared form.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"

namespace agentty::ui {

Element web_search_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::WebSearch>();
    if (!o) return nothing();
    // The scroll offset is the slot's own (pn::WebSearch::scroll), so it is
    // hashed with the panel and starts at the top every time it opens.
    return maya::Panel{form_config(o->form, info,
                                  &o->scroll,
                                  panel_viewport_h(),
                                  panel_terminal_cols())}.build();
}

} // namespace agentty::ui
