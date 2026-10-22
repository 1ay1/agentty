// paste_settings.cpp — Settings → Paste: how Ctrl+V reads the clipboard.
//
// One form pane over the paste.* registry rows. A committed edit writes
// m.d.persisted.paste, returns the save, and rebuilds the rows from the Model.
// Telling the clipboard reader is publish_derived's job (PublishPasteConfig),
// the same way Web Search's policy reaches the tool layer.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <string>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "agentty/runtime/settings_registry.hpp"

namespace pn  = agentty::ui::panel;
namespace reg = agentty::settings::registry;

namespace agentty::app::detail {

using maya::overload;

form::Form paste_settings_form(const paste_cfg::Config& c, const reg::EnvSnapshot& env) {
    form::Builder b{" Paste "};
    b.subtitle("how Ctrl+V reads the clipboard \xc2\xb7 `agentty paste doctor` shows the route it takes");
    bool first = true;
    auto last_group = reg::Group::PasteRoutes;
    // Nine rows: show them all rather than hide the advanced ones.
    reg::add_rows(b, c, env, reg::Owner::Paste, /*advanced=*/true, first, last_group);
    return b.build();
}

form::Form paste_form_for_search(const Model& m) {
    return paste_settings_form(m.d.persisted.paste, m.env.settings);
}

namespace {

void rebuild(const Model& m, pn::PasteSettings& o) {
    form::rebuild(o.form, paste_settings_form(m.d.persisted.paste, m.env.settings));
}

} // namespace

Cmd paste_settings_update(Model& m, msg::PasteSettingsMsg pm) {
    return std::visit(overload{
        [&](OpenPasteSettings& e) -> Cmd {
            pn::PasteSettings o;
            o.form = paste_settings_form(m.d.persisted.paste, m.env.settings);
            (void)form::focus_row(o.form, e.focus);
            m.ui.panel.descend(std::move(o));
            return Cmd::none();
        },
        [&](ClosePasteSettings) -> Cmd {
            ascend(m);
            return Cmd::none();
        },
        [&](PasteSettingsPaste& e) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::PasteSettings>())
                (void)form::paste_into_focused(o->form, e.text);
            return Cmd::none();
        },
        [&](PasteSettingsKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::PasteSettings>();
            if (!o) return Cmd::none();
            const auto applied = form::keys::apply(o->form, e.action);
            if (applied.close)
                return paste_settings_update(m, msg::PasteSettingsMsg{ClosePasteSettings{}});
            // Mid-edit keystrokes must leave the form alone (see web_search.cpp).
            if (applied.settled < 0) return Cmd::none();

            paste_cfg::Config cfg = m.d.persisted.paste;
            reg::read_rows(cfg, o->form, reg::Owner::Paste);
            if (cfg == m.d.persisted.paste) {
                rebuild(m, *o);
                return Cmd::none();
            }
            m.d.persisted.paste = std::move(cfg);
            Cmd out = save_record(m);
            rebuild(m, *o);
            o->form.dirty = false;   // just saved: don't say "unsaved"
            return out;
        },
    }, pm);
}

} // namespace agentty::app::detail
