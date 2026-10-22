// paste_settings.cpp — the Settings → Paste panel, via the shared form.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"

namespace agentty::ui {

Element paste_settings_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::PasteSettings>();
    if (!o) return nothing();
    return maya::Panel{form_config(o->form, info,
                                  &o->scroll,
                                  panel_viewport_h(),
                                  panel_terminal_cols())}.build();
}

} // namespace agentty::ui
