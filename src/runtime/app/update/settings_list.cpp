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
#include "agentty/tool/registry.hpp"   // reload_mcp_plugins

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
            // Restore the cursor to the row that opened this picker so the
            // palette comes back exactly where the user left it.
            Command src = Command::OpenPlugins;
            if (auto* o = settings_list_opened(m.ui.settings_list)) {
                switch (o->concern) {
                    case se::Category::Plugins:  src = Command::OpenPlugins;  break;
                    case se::Category::Commands: src = Command::OpenCommands; break;
                    case se::Category::Agents:   src = Command::OpenAgents;   break;
                    case se::Category::Hooks:    src = Command::OpenHooks;    break;
                    case se::Category::General:  src = Command::OpenPlugins;  break;
                }
            }
            m.ui.settings_list = se::ListClosed{};
            m.ui.command_palette = palette::Open{"", palette_index_of(src)};
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
                    maya::Cmd<Msg> cmd;
                    if (r == tools::plugin::EditResult::Ok) {
                        // Reload OFF the UI thread — the connect handshake
                        // can take a beat and must never freeze the TUI.
                        // The config write already happened synchronously,
                        // so the reload just re-syncs the live pool to disk.
                        cmd = maya::Cmd<Msg>::batch(std::vector<maya::Cmd<Msg>>{
                            maya::Cmd<Msg>::task_isolated(
                                [](std::function<void(Msg)> dispatch) {
                                    (void)tools::reload_mcp_plugins();
                                    dispatch(SettingsListReloaded{}); // repaint
                                }),
                            set_status_toast(m, "removed plugin '" + row.arg +
                                                "' — disconnected")});
                    } else {
                        cmd = set_status_toast(m,
                                  "could not remove '" + row.arg + "'");
                    }
                    // Re-clamp: the list shrank by one.
                    if (auto* oo = settings_list_opened(m.ui.settings_list)) {
                        const int n = static_cast<int>(
                            se::items_for(m, oo->concern).size());
                        oo->index = std::clamp(oo->index, 0, std::max(0, n - 1));
                    }
                    return {std::move(m), std::move(cmd)};
                }
                case se::Action::ToggleTool: {
                    // Enable/disable one tool of a plugin: persist to
                    // mcp.json's tools.exclude, then invalidate the wire
                    // catalog so it re-projects with the new filter. NO
                    // server re-spawn, NO background thread — the server
                    // stays connected and project_tools re-reads the live
                    // exclude. This is synchronous + race-free (the earlier
                    // reload-on-toggle re-spawned the server and hung on
                    // rapid disable→re-enable).
                    auto path = tools::plugin::config_path(/*project=*/false);
                    const bool want_enabled = !row.on;   // toggle
                    auto r = tools::plugin::set_tool_enabled(
                        path, row.arg, row.arg2, want_enabled);
                    maya::Cmd<Msg> cmd;
                    if (r == tools::plugin::EditResult::Ok) {
                        tools::invalidate_mcp_catalog();
                        cmd = set_status_toast(m,
                            (want_enabled ? "enabled " : "disabled ")
                            + row.arg + "__" + row.arg2);
                    } else {
                        cmd = set_status_toast(m,
                            "could not toggle '" + row.arg2 + "'");
                    }
                    // Keep the cursor on the same row after the list rebuilds.
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
        [&](SettingsListPaste& e) -> Step {
            auto* o = settings_list_opened(m.ui.settings_list);
            if (!o || !o->input_active) return done(std::move(m));
            // The add-prompt is a single line (a plugin's "name command
            // args…" spec, or a new file's name). Flatten any newlines/tabs
            // in the paste to spaces so a multi-line clipboard can't smuggle
            // a line break into the one-line field or split the arg vector.
            std::string clean;
            clean.reserve(e.text.size());
            for (char c : e.text)
                clean += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
            o->input.insert(static_cast<std::size_t>(o->cursor), clean);
            o->cursor += static_cast<int>(clean.size());
            return done(std::move(m));
        },
        [&](SettingsListReloaded) -> Step {
            // A background plugin reload just finished. Bump the nonce so the
            // Model genuinely changes and the TEA loop repaints the panel;
            // the view re-runs items_for() → plugin_model(), which now
            // reports the server as connected (or errored) instead of the
            // stale "connecting…". No-op if the panel was closed meanwhile.
            if (auto* o = settings_list_opened(m.ui.settings_list))
                ++o->reload_nonce;
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

            // Make it USABLE NOW, not after a restart. For Plugins the
            // add already wrote mcp.json; reload the live pool OFF the UI
            // thread (the connect handshake must never freeze the TUI —
            // the bridge bounds it with a deadline). Commands are loaded
            // fresh per use (create_starter invalidated the cache) and
            // agents are scanned per task-tool call, so both are already
            // live — no reload needed.
            maya::Cmd<Msg> reload = maya::Cmd<Msg>::none();
            if (r.ok && concern == se::Category::Plugins) {
                reload = maya::Cmd<Msg>::task_isolated(
                    [](std::function<void(Msg)> dispatch) {
                        (void)tools::reload_mcp_plugins();
                        // The pool is now rebuilt (the new server is
                        // connected/failed for real). Kick the reducer so
                        // the Plugins panel re-renders from the fresh
                        // plugin_model() — otherwise it stays on the stale
                        // "connecting…" snapshot until the next unrelated
                        // event, which reads as a permanent hang.
                        dispatch(SettingsListReloaded{});
                    });
                r.message += " — connecting…";
            }
            // Re-clamp the (possibly grown) list to the top of the new row.
            if (auto* oo = settings_list_opened(m.ui.settings_list)) {
                const int cnt =
                    static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, cnt - 1));
            }
            return {std::move(m), maya::Cmd<Msg>::batch(
                std::vector<maya::Cmd<Msg>>{
                    std::move(reload), set_status_toast(m, r.message)})};
        },
    }, sm);
}

} // namespace agentty::app::detail
