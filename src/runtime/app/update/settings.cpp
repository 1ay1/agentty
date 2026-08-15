// settings_update — reducer for the Settings pane (Ctrl+K → Settings).
//
// Navigation model: two columns. Focus starts on Categories; →/Tab moves
// to Items, ← back. ↑↓ move within the focused column. Enter on a category
// (while focused there) jumps focus into its items; Enter on an item runs
// its Action (cycle profile, open a sub-picker, remove a plugin, approve
// hooks). Sub-pickers (RAG/Smart) re-enter the top-level update() exactly
// like the palette does, and close the pane first so overlays don't stack.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/settings_items.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/hooks.hpp"
#include "agentty/domain/profile.hpp"

namespace agentty::app::detail {

using maya::overload;
namespace se = agentty::settings;

namespace {

// Clamp item_index into the live row count of the current category.
void clamp_items(Model& m, se::Open& o) {
    const int n = static_cast<int>(se::items_for(m, o.cat).size());
    if (n == 0) { o.item_index = 0; return; }
    o.item_index = std::clamp(o.item_index, 0, n - 1);
}

} // namespace

Step settings_update(Model m, msg::SettingsMsg sm) {
    return std::visit(overload{
        [&](OpenSettings) -> Step {
            m.ui.settings = se::Open{};
            return done(std::move(m));
        },
        [&](CloseSettings) -> Step {
            m.ui.settings = se::Closed{};
            return done(std::move(m));
        },
        [&](SettingsFocus& e) -> Step {
            auto* o = settings_opened(m.ui.settings);
            if (!o) return done(std::move(m));
            // → into items, ← back to categories. Never leaves a column
            // "focused" on an empty item list — a category with rows always
            // has ≥1.
            if (e.delta > 0) {
                o->focus = se::Focus::Items;
                clamp_items(m, *o);
            } else {
                o->focus = se::Focus::Categories;
            }
            return done(std::move(m));
        },
        [&](SettingsMove& e) -> Step {
            auto* o = settings_opened(m.ui.settings);
            if (!o) return done(std::move(m));
            if (o->focus == se::Focus::Categories) {
                o->cat_index = std::clamp(o->cat_index + e.delta, 0,
                                          se::kCategoryCount - 1);
                o->cat = se::kCategories[o->cat_index];
                o->item_index = 0;   // reset row cursor on category change
            } else {
                const int n =
                    static_cast<int>(se::items_for(m, o->cat).size());
                if (n > 0)
                    o->item_index =
                        std::clamp(o->item_index + e.delta, 0, n - 1);
            }
            return done(std::move(m));
        },
        [&](SettingsActivate) -> Step {
            auto* o = settings_opened(m.ui.settings);
            if (!o) return done(std::move(m));
            // Enter while on the category column: dive into its items.
            if (o->focus == se::Focus::Categories) {
                o->focus = se::Focus::Items;
                clamp_items(m, *o);
                return done(std::move(m));
            }
            // Enter on an item: resolve via the SAME live list the view
            // rendered (index-into-live, never a stale enum).
            auto rows = se::items_for(m, o->cat);
            if (o->item_index < 0 ||
                o->item_index >= static_cast<int>(rows.size()))
                return done(std::move(m));
            const se::Item& row = rows[static_cast<std::size_t>(o->item_index)];

            switch (row.action) {
                case se::Action::CycleProfile:
                    return agentty::app::update(std::move(m), Msg{CycleProfile{}});
                case se::Action::OpenRag:
                    m.ui.settings = se::Closed{};
                    return agentty::app::update(std::move(m), Msg{OpenRagSettings{}});
                case se::Action::OpenSmart:
                    m.ui.settings = se::Closed{};
                    return agentty::app::update(std::move(m), Msg{OpenSmartMode{}});
                case se::Action::RemovePlugin: {
                    auto path = tools::plugin::config_path(/*project=*/false);
                    auto r = tools::plugin::remove_server(path, row.arg);
                    auto cmd = (r == tools::plugin::EditResult::Ok)
                        ? set_status_toast(m, "removed plugin '" + row.arg +
                              "' — restart to disconnect")
                        : set_status_toast(m, "could not remove '" + row.arg + "'");
                    clamp_items(m, *o);
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ApproveHooks: {
                    // The pane can't do the interactive y/N prompt safely
                    // (it owns the screen), so it points at the CLI — the
                    // approval MUST be a deliberate terminal action, by
                    // design. Toast the exact command.
                    return {std::move(m), set_status_toast(m,
                        "run `agentty hooks approve` in a shell to review "
                        "+ approve (consent is deliberate by design)")};
                }
                case se::Action::None:
                default:
                    return done(std::move(m));
            }
        },
    }, sm);
}

} // namespace agentty::app::detail
