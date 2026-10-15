// smart_mode.cpp — the Smart Mode pane's reducer: open/close (descend/
// ascend), the advanced-rows toggle, key dispatch through the shared form
// layer, the slot hand-off to the model picker, and slot reset.
//
// build_smart_form / apply_smart stay in meta.cpp (shared with models.cpp
// and ascend()); declared in internal.hpp.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/smart_form.hpp"
#include "agentty/runtime/settings_registry.hpp"   // tuning row write-back

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Cmd smart_mode_update(Model& m, msg::SmartModeMsg sm) {
    return std::visit(overload{
        [&](OpenSmartMode& e) -> Cmd {
            // descend(): whatever is open right now (palette, settings list,
            // nothing) becomes this pane's Esc target automatically — no
            // caller stamps an origin any more. A search hit on an advanced
            // row opens with those rows shown.
            const auto* d = settings::registry::find(e.focus);
            const bool advanced = d && d->tier == settings::registry::Tier::Advanced;
            m.ui.panel.descend(pn::SmartMode{{}, build_smart_form(m, advanced), advanced});
            (void)form::focus_row(m.ui.panel.get<pn::SmartMode>()->form, e.focus);
            return Cmd::none();
        },
        [&](SmartModeAdvanced) -> Cmd {
            // Reveal/hide the routing-policy rows. The row SET changes, so the
            // form is rebuilt; the cursor is kept and clamped, since hiding
            // rows can leave it past the end.
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return Cmd::none();
            o->advanced = !o->advanced;
            form::rebuild(o->form, build_smart_form(m, o->advanced));
            return Cmd::none();
        },
        [&](CloseSmartMode) -> Cmd {
            // Esc unwinds ONE level, to whatever opened this pane — ^S from
            // the thread closes; a palette row or the settings list is
            // restored with its full state (query, cursor) intact.
            ascend(m);
            return Cmd::none();
        },
        [&](SmartModePaste& e) -> Cmd {
            // A paste lands in an edit session; it commits when that edit
            // ends, the same as typed digits.
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (o) form::paste_into_focused(o->form, e.text);
            return Cmd::none();
        },
        [&](SmartModeKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return Cmd::none();

            // What the cursor is on BEFORE the shared reducer runs — activate
            // may hand off, and we need to know which row asked.
            const auto* row = o->form.focused();
            const std::string row_id = row ? row->id : std::string{};
            const auto role = smart_form::role_of_field(row_id);

            const auto applied = form::keys::apply(o->form, e.action);

            if (applied.close) {
                // Route through CloseSmartMode rather than closing here: that
                // is where the origin is read, so a direct close would send
                // Esc to the thread no matter how the pane was opened. Two
                // ways to close one pane is two behaviours to keep in step.
                return smart_mode_update(m, msg::SmartModeMsg{CloseSmartMode{}});
            }

            // Commit once a value settles (in-place change, or the end of a
            // typed edit). Committing on every digit rebuilt the form mid-edit
            // and threw the edit away.
            const std::string settled_id = applied.settled >= 0
                ? o->form.fields[static_cast<std::size_t>(applied.settled)].id
                : std::string{};
            const auto settled_role = smart_form::role_of_field(settled_id);
            if (!settled_id.empty() && !settled_role) {
                if (const auto* d = settings::registry::find(settled_id)) {
                    smart::RoleConfig cfg = m.d.smart;
                    if (const auto* f = o->form.find(settled_id))
                        (void)settings::registry::read_row(cfg, *d, *f);
                    auto save = apply_smart(m, std::move(cfg));
                    form::rebuild(o->form, build_smart_form(m, o->advanced));
                    return save;
                }
                if (settled_id != smart_form::kFieldEnabled) return Cmd::none();
                // The master toggle. Routed through apply_smart like every
                // other config change, so the subagent router is pushed too —
                // turning Smart Mode on while a `task` was queued used to leave
                // that worker routing as if it were still off.
                smart::RoleConfig cfg = m.d.smart;
                cfg.enabled = smart_form::enabled_from_form(o->form,
                                                            m.d.smart.enabled);
                auto save = apply_smart(m, std::move(cfg));
                // The slots' locked state depends on the switch, so rebuild.
                form::rebuild(o->form, build_smart_form(m, o->advanced));
                return Cmd::batch(std::move(save), set_status_toast(m,
                    m.d.smart.enabled ? "Smart Mode on" : "Smart Mode off"));
            }

            // A slot row → hand off to the model picker. The candidate set is
            // the whole catalogue, which is exactly what a `Pick` row means:
            // too large and too dynamic for an inline dropdown.
            if (applied.hand_off && role) {
                // descend(): the ENTIRE SmartMode pane — form, cursor,
                // advanced flag, its own parent chain — rides in the
                // picker's `from` snapshot; Esc or a completed pin restores
                // it verbatim. Nothing is parked on Model::UI any more.
                pn::Models picker{{0, ""}, {}, *role};
                m.ui.panel.descend(std::move(picker));
                return models_update(m, msg::ModelsMsg{OpenModels{}});
            }

            // 'x' resets the focused slot to auto.
            if (e.action.intent == form::keys::Intent::ResetField && role) {
                return smart_mode_update(m, msg::SmartModeMsg{SmartModeClearSlot{}});
            }
            return Cmd::none();
        },
        [&](SmartModeClearSlot) -> Cmd {
            auto* o = m.ui.panel.get<pn::SmartMode>();
            if (!o) return Cmd::none();
            // `x` only means something on a slot row. role_of_field returns
            // nullopt for the master switch, so "not a slot" cannot fall
            // through into a slot the way an int comparison could.
            const auto* row = o->form.focused();
            if (!row || row->locked) return Cmd::none();
            const auto role = smart_form::role_of_field(row->id);
            if (!role) return Cmd::none();
            // Reset to auto through the ONE entry point, so the subagent
            // router loses the pin too. Clearing a slot but leaving a worker
            // routing on it is the same class of bug as the save-without-apply.
            smart::RoleConfig cfg = m.d.smart;
            cfg.slot(*role) = smart::SlotOverride{};
            auto save = apply_smart(m, std::move(cfg));
            // The row shows the RESOLVED model, which just changed.
            form::rebuild(o->form, build_smart_form(m, o->advanced));
            return Cmd::batch(std::move(save),
                              set_status_toast(m, "slot reset to auto"));
        },
    }, sm);
}

} // namespace agentty::app::detail
