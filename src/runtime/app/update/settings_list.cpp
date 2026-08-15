// settings_list_update — reducer for the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list modal parameterised by the
// concern it was opened with. ↑↓ move over the live rows; Enter runs the
// focused row's Action; Esc closes. Row resolution is index-into-live
// (settings::items_for), never a stale enum — the same correctness rule
// the command palette uses.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/settings_items.hpp"
#include "agentty/runtime/view/helpers.hpp"   // utf8_encode / utf8_prev
#include "agentty/tool/plugin.hpp"

namespace agentty::app::detail {

using maya::overload;
namespace se = agentty::settings;

Step settings_list_update(Model m, msg::SettingsListMsg sm) {
    return std::visit(overload{
        [&](OpenSettingsList& e) -> Step {
            m.ui.settings_list = se::ListOpen{e.concern, 0};
            return done(std::move(m));
        },
        [&](CloseSettingsList) -> Step {
            // Esc returns to the command palette the user opened this from,
            // rather than dropping straight back to the thread — the
            // settings pickers are reached via Ctrl+K, so Esc unwinds one
            // level (picker → palette → thread), matching the mental stack.
            m.ui.settings_list = se::ListClosed{};
            m.ui.command_palette = palette::Open{};
            return done(std::move(m));
        },
        [&](SettingsListMove& e) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o) return done(std::move(m));
            const int n =
                static_cast<int>(se::items_for(m, o->concern).size());
            if (n <= 0) { o->index = 0; return done(std::move(m)); }
            o->index = std::clamp(o->index + e.delta, 0, n - 1);
            return done(std::move(m));
        },
        [&](SettingsListActivate) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o) return done(std::move(m));
            auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return done(std::move(m));
            const se::Item& row = rows[static_cast<std::size_t>(o->index)];

            switch (row.action) {
                case se::Action::CycleProfile:
                    return agentty::app::update(std::move(m), Msg{CycleProfile{}});
                case se::Action::OpenRag:
                    m.ui.settings_list = se::ListClosed{};
                    return agentty::app::update(std::move(m), Msg{OpenRagSettings{}});
                case se::Action::OpenSmart:
                    m.ui.settings_list = se::ListClosed{};
                    return agentty::app::update(std::move(m), Msg{OpenSmartMode{}});
                case se::Action::RemovePlugin: {
                    auto path = tools::plugin::config_path(/*project=*/false);
                    auto r = tools::plugin::remove_server(path, row.arg);
                    auto cmd = (r == tools::plugin::EditResult::Ok)
                        ? set_status_toast(m, "removed plugin '" + row.arg +
                              "' \u2014 restart to disconnect")
                        : set_status_toast(m, "could not remove '" + row.arg + "'");
                    // Re-clamp: the list shrank by one.
                    if (auto* oo = settings_list_opened(m.ui.settings_list)) {
                        const int n = static_cast<int>(
                            se::items_for(m, oo->concern).size());
                        oo->index = std::clamp(oo->index, 0, std::max(0, n - 1));
                    }
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ApproveHooks:
                    // Consent MUST be a deliberate terminal action — the
                    // picker can't safely own the y/N prompt while it holds
                    // the screen. Point at the CLI.
                    return {std::move(m), set_status_toast(m,
                        "run `agentty hooks approve` in a shell to review "
                        "+ approve (consent is deliberate by design)")};
                case se::Action::None:
                default:
                    return done(std::move(m));
            }
        },
        [&](SettingsListAddStart) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o) return done(std::move(m));
            // Add is only meaningful for the file/config-backed concerns.
            if (o->concern != se::Category::Plugins
                && o->concern != se::Category::Commands
                && o->concern != se::Category::Agents)
                return done(std::move(m));
            o->input_active = true;
            o->input.clear();
            o->cursor = 0;
            return done(std::move(m));
        },
        [&](SettingsListChar& e) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o || !o->input_active) return done(std::move(m));
            const std::string utf8 = ui::utf8_encode(e.ch);
            o->input.insert(static_cast<std::size_t>(o->cursor), utf8);
            o->cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](SettingsListBackspace) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o || !o->input_active || o->cursor <= 0)
                return done(std::move(m));
            int p = ui::utf8_prev(o->input, o->cursor);
            o->input.erase(static_cast<std::size_t>(p),
                           static_cast<std::size_t>(o->cursor - p));
            o->cursor = p;
            return done(std::move(m));
        },
        [&](SettingsListCancelInput) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o) return done(std::move(m));
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            return done(std::move(m));
        },
        [&](SettingsListSubmitInput) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o || !o->input_active) return done(std::move(m));
            const std::string line = o->input;
            const se::Category concern = o->concern;
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            if (line.empty()) return done(std::move(m));   // empty = cancel

            se::AddResult r = (concern == se::Category::Plugins)
                ? se::add_plugin_from_line(line)
                : se::create_starter(concern, line);
            // Re-clamp the (possibly grown) list to the top of the new row.
            if (auto* oo = settings_list_opened(m.ui.settings_list)) {
                const int cnt =
                    static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, cnt - 1));
            }
            return {std::move(m), set_status_toast(m, r.message)};
        },
    }, sm);
}

} // namespace agentty::app::detail
