// settings_list_update — reducer for the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list modal parameterised by the
// concern it was opened with. ↑↓ move over the live rows; Enter runs the
// focused row's Action; Esc closes. Row resolution is index-into-live
// (settings::items_for), never a stale enum — the same correctness rule
// the command palette uses.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/domain/ui_theme.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"   // load_plugins_async

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/panel/step.hpp"
#include "agentty/runtime/view/helpers.hpp"   // utf8_encode / utf8_prev
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/registry.hpp"   // reload_mcp_plugins

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace {
// Re-enter the whole-Msg reducer, in place.
//
// These rows dispatch across FIVE different domains (profile, rag, smart,
// appearance, meta), which is exactly why they go through the top-level
// reducer instead of naming a domain reducer each: the row's action IS the
// dispatch table. update() still returns a pair, so unpack it here — one
// place rather than at every row.
[[nodiscard]] Cmd reenter(Model& m, Msg msg) {
    auto [next, cmd] = agentty::app::update(std::move(m), std::move(msg));
    m = std::move(next);
    return std::move(cmd);
}
} // namespace


using maya::overload;
namespace se = agentty::settings;
namespace cmdf = agentty::app::cmd;   // cmd_factory (local vars named `cmd` shadow the ns)

namespace {
// The mcp.json an edit on THIS row should write. A server row carries the
// .agentty dir it was read from (scope provenance from PluginModel), so a
// remove/toggle lands in the RIGHT file — a project server edits the project
// config, a user server the user config. Empty config_dir (add-mode, or a
// pre-provenance row) falls back to the user config, the historical default.
[[nodiscard]] std::filesystem::path edit_target(const se::Item& row, const Model& m) {
    if (!row.config_dir.empty())
        return std::filesystem::path{row.config_dir} / "mcp.json";
    return tools::plugin::config_path(/*project=*/false, m.env.user_root);
}

// True when row `i` is one the user can act on (Enter/toggle/etc).
[[nodiscard]] bool actionable_at(const std::vector<se::Item>& rows, int i) {
    return i >= 0 && i < static_cast<int>(rows.size())
        && rows[static_cast<std::size_t>(i)].action != se::Action::None;
}

// Cursor-landing SSOT for the settings list: the index of the first row the
// user can ACT on, scanning from `start` in `dir` (+1 down / -1 up), `start`
// included. Skips Action::None rows (headers, built-in listings, empty-state
// placeholders). If nothing actionable exists that way, returns `start`
// clamped — so a wholly-informational pane (e.g. Agents built-ins) still has
// a valid, stable cursor and never lands out of range. Used by open, move,
// and the plugins-refresh reconciler so "where the cursor sits" is decided in
// ONE place.
[[nodiscard]] int first_actionable(const std::vector<se::Item>& rows,
                                   int start, int dir) {
    const int n = static_cast<int>(rows.size());
    if (n <= 0) return 0;
    start = std::clamp(start, 0, n - 1);
    const int step = dir < 0 ? -1 : 1;
    for (int i = start; i >= 0 && i < n; i += step)
        if (rows[static_cast<std::size_t>(i)].action != se::Action::None)
            return i;
    return start;
}

// Open the pane a search hit lives in, on that row. Descends, so Esc comes
// back to the search with its query.
[[nodiscard]] Msg open_hit(const se::ListOpen::Hit& h) {
    using P = se::ListOpen::Pane;
    switch (h.pane) {
        case P::Rag:        return Msg{OpenRag{h.row}};
        case P::Smart:      return Msg{OpenSmartMode{h.row}};
        case P::Appearance: return Msg{OpenAppearance{h.row}};
        case P::Sandbox:    return Msg{OpenSandbox{h.row}};
        case P::WebSearch:  return Msg{OpenWebSearch{h.row}};
    }
    return Msg{OpenRag{h.row}};
}
}  // namespace

// Every setting in every form pane, from each pane's own form builder.
std::vector<se::ListOpen::Hit> settings_search_index(const Model& m) {
    using P = se::ListOpen::Pane;
    std::vector<se::ListOpen::Hit> out;
    se::index_form(out, rag_form_for_search(m),        P::Rag);
    se::index_form(out, build_smart_form(m, true),     P::Smart);
    se::index_form(out, appearance_form_for_search(m), P::Appearance);
    se::index_form(out, sandbox_form_for_search(m),    P::Sandbox);
    se::index_form(out, web_search_form_for_search(m), P::WebSearch);
    return out;
}

Cmd settings_list_update(Model& m, msg::SettingsListMsg sm) {
    return std::visit(overload{
        [&](OpenSettingsList& e) -> Cmd {
            // Land the cursor on the first ACTIONABLE row, not a leading
            // header (the Plugins pane opens with a "N tools on the wire"
            // info row). first_actionable clamps to a valid index for a
            // wholly-informational / empty pane too.
            const int start =
                first_actionable(se::items_for(m, e.concern), 0, +1);
            // descend(), not assignment.
            //
            // Assignment overwrites whatever was open, so the pane Esc came
            // FROM is gone and Esc closes the whole stack instead of
            // stepping back one. Reached from Ctrl+K the palette's adopt()
            // used to paper over this; reached any other way — a direct
            // key, another pane handing off — there was nothing to adopt and
            // Esc dropped the user on the thread. descend() stashes the
            // parent at the open site, which is the one place that always
            // knows what it is replacing.
            // A different list opened from a list (the hub's doors) is a step
            // down, so Esc returns to the hub; reopening the same list is a
            // rebuild.
            const auto* cur = m.ui.panel.get<pn::SettingsList>();
            if (cur && cur->concern != e.concern)
                m.ui.panel.push(pn::SettingsList{{e.concern, start}});
            else
                m.ui.panel.descend(pn::SettingsList{{e.concern, start}});
            // A fresh open starts at the top — don't inherit the scroll
            // offset of the last visit (the widget's keep-selection-in-view
            // would fight the stale offset for a frame). PR #34.
            m.ui.settings_list_scroll.y = 0;
            if (e.concern == se::Category::General)
                if (auto* hub = m.ui.panel.get<pn::SettingsList>())
                    hub->index_rows = settings_search_index(m);
            // Opening the Plugins panel is what CONNECTS the servers (and
            // refreshes the snapshot): the connection is driven by the update
            // loop, not a lazy side effect of the first tool call. Without
            // this the panel showed "connecting…" forever until you happened
            // to send a turn. Fire the async connect; PluginsUpdated lands
            // the result in m.ui.plugins.
            if (e.concern == se::Category::Plugins) {
                // If the startup connect (from init) is still in flight, don't
                // fire a SECOND one — just show its spinner. Redundant
                // concurrent connects were the "date connected 4×" symptom
                // and the precondition for the pool-swap race. When idle,
                // opening the panel is what (re)connects.
                if (!m.ui.plugins_loading) {
                    m.ui.plugins_loading = true;
                    return cmdf::load_plugins_async(/*reconnect=*/true);
                }
            }
            return Cmd::none();
        },
        [&](PluginsUpdated& e) -> Cmd {
            // The connect/reload finished on a worker. Store the snapshot
            // (this IS the model delta that repaints the panel — visual_hash
            // covers m.ui.plugins, so no nonce hack) and clear the spinner.
            //
            // Cursor IDENTITY, not index: a reload can add/remove/reorder
            // rows, so a raw index clamp would silently slide the cursor to a
            // neighbour — and a later Enter/`d` would act on the wrong server.
            // Remember which row (by server+tool+action) was selected BEFORE
            // the swap, then restore the cursor to that same row after.
            std::optional<std::tuple<std::string, std::string, se::Action>> sel;
            if (auto* o = m.ui.panel.get<pn::SettingsList>();
                o && o->concern == se::Category::Plugins) {
                auto before = se::items_for(m, o->concern);
                if (o->index >= 0 && o->index < static_cast<int>(before.size())) {
                    const auto& r = before[static_cast<std::size_t>(o->index)];
                    sel = std::make_tuple(r.arg, r.arg2, r.action);
                }
            }

            m.ui.plugins = std::move(e.model);
            m.ui.plugins_loading = false;

            if (auto* o = m.ui.panel.get<pn::SettingsList>();
                o && o->concern == se::Category::Plugins) {
                auto after = se::items_for(m, o->concern);
                const int cnt = static_cast<int>(after.size());
                int restored = -1;
                if (sel) {
                    for (int i = 0; i < cnt; ++i)
                        if (after[static_cast<std::size_t>(i)].arg == std::get<0>(*sel)
                            && after[static_cast<std::size_t>(i)].arg2 == std::get<1>(*sel)
                            && after[static_cast<std::size_t>(i)].action == std::get<2>(*sel)) {
                            restored = i; break;
                        }
                }
                // Same row found → follow it; otherwise re-land on the
                // first actionable row at/after the old index (never a stale
                // header) — the row it named is gone (e.g. removed server).
                o->index = restored >= 0
                    ? restored
                    : first_actionable(after,
                          std::clamp(o->index, 0, std::max(0, cnt - 1)), +1);
            }
            return Cmd::none();
        },
        [&](CloseSettingsList) -> Cmd {
            // Esc unwinds one level: the palette snapshot stashed at open
            // (query and cursor intact), or the thread when ^S-style direct
            // entry left no parent. The old hand-reconstruction mapped the
            // category back to a palette command; the snapshot just IS the
            // palette the user left.
            ascend(m);
            return Cmd::none();
        },
        [&](SettingsListMove& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return Cmd::none();
            o->confirm_remove.clear();   // moving off a row disarms a pending ^D
            if (se::is_search(*o)) {
                const int hits = static_cast<int>(se::search(o->index_rows, o->query).size());
                o->index = ui::step(o->index, e.delta, hits);
                return Cmd::none();
            }
            auto rows = se::items_for(m, o->concern);
            const int n = static_cast<int>(rows.size());
            if (n <= 0) { o->index = 0; return Cmd::none(); }

            // Land only on rows you can act on, and wrap at the ends like
            // every list. A pane with no actionable row still scrolls.
            const int idx = std::clamp(o->index, 0, n - 1);
            bool any = false;
            for (int i = 0; i < n; ++i) any = any || actionable_at(rows, i);
            o->index = any
                ? ui::step_over(idx, e.delta, n,
                                [&](int i) { return !actionable_at(rows, i); })
                : ui::step(idx, e.delta, n);
            return Cmd::none();
        },
        [&](SettingsListActivate) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return Cmd::none();
            o->confirm_remove.clear();   // any Enter action disarms a pending ^D
            if (se::is_search(*o)) {
                const auto hits = se::search(o->index_rows, o->query);
                if (o->index < 0 || o->index >= static_cast<int>(hits.size()))
                    return Cmd::none();
                const auto& h = o->index_rows[static_cast<std::size_t>(
                    hits[static_cast<std::size_t>(o->index)])];
                return reenter(m, open_hit(h));
            }
            auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return Cmd::none();
            const se::Item& row = rows[static_cast<std::size_t>(o->index)];

            switch (row.action) {
                case se::Action::CycleProfile:
                    return reenter(m, Msg{CycleProfile{}});
                case se::Action::OpenRag: {
                    // Descending, not jumping: OpenRag runs with the
                    // list still open, so descend() stashes it — category,
                    // cursor and its own parent chain — as the pane's Esc
                    // target. No hand-stamped origin, nothing to forget.
                    return reenter(m, Msg{OpenRag{}});
                }
                case se::Action::OpenSmart: {
                    return reenter(m, Msg{OpenSmartMode{}});
                }
                case se::Action::OpenAppearance: {
                    return reenter(m, Msg{OpenAppearance{}});
                }
                case se::Action::OpenSandbox: {
                    return reenter(m, Msg{OpenSandbox{}});
                }
                case se::Action::OpenWebSearch: {
                    return reenter(m, Msg{OpenWebSearch{}});
                }
                case se::Action::OpenList:
                    // OpenSettingsList descends, so Esc comes back here.
                    return reenter(m, Msg{OpenSettingsList{row.target}});
                case se::Action::ToggleChangesStrip: {
                    return reenter(m, Msg{ToggleChangesStrip{}});
                }
                case se::Action::ToggleAutoTitle: {
                    return reenter(m, Msg{ToggleAutoTitle{}});
                }
                // (There is deliberately NO Activate-arm for plugin removal.
                // Removal is `d` → SettingsListRemove, which is TWO-step —
                // arm, then confirm. An Enter-fired remove here would be a
                // one-press destructive action; the old arm was unreachable
                // — no row builder ever emitted it — but unreachable code
                // with live side effects is a trap, not a feature.)

                case se::Action::TogglePlugin: {
                    // Ignore a toggle while a connect/reload is already in
                    // flight — the snapshot (and this row's on/off) is mid-
                    // change, so acting on it could write a stale intent.
                    if (m.ui.plugins_loading)
                        return Cmd::none();
                    // Enter on a plugin row flips the WHOLE server on/off — a
                    // reversible `disabled` flag in mcp.json, NOT a delete.
                    // Enabling spawns + handshakes; disabling drops the
                    // connection. Either way it changes the live process set,
                    // so reconnect=true, off the UI thread.
                    //
                    // The write is an effect (cmd::edit_plugin); its outcome
                    // arrives as PluginEdited, handled below. plugins_loading
                    // goes up NOW, at the request, so the guard above ignores
                    // a second Enter while the write is in flight instead of
                    // racing it with a stale intent.
                    m.ui.plugins_loading = true;
                    return cmdf::edit_plugin(edit_target(row, m), PluginEdited{
                        .kind   = PluginEdited::Kind::SetServerDisabled,
                        .from   = PluginEdited::From::SettingsList,
                        .server = row.arg,
                        .flag   = row.on,   // on → turn off
                    });
                }
                case se::Action::ToggleTool: {
                    if (m.ui.plugins_loading)
                        return Cmd::none();
                    // Toggling one tool of a DISABLED/disconnected plugin is
                    // meaningless — the whole plugin runs nothing. Guide the
                    // user to enable the plugin (Enter on the parent) instead
                    // of silently editing an exclude that has no effect.
                    if (row.inactive)
                        return set_status_toast(m,
                            "'" + row.arg + "' is disabled — enable the plugin "
                            "first (Enter on it), then toggle its tools");
                    // Enable/disable one tool of a plugin: persist to
                    // mcp.json's tools.exclude, then invalidate the wire
                    // catalog so it re-projects with the new filter. NO
                    // server re-spawn — the server stays connected and
                    // project_tools re-reads the live exclude (the earlier
                    // reload-on-toggle re-spawned the server and hung on
                    // rapid disable→re-enable).
                    //
                    // The write and the catalog invalidation it requires both
                    // happen on the worker (cmd::edit_plugin); PluginEdited
                    // brings the result back.
                    m.ui.plugins_loading = true;
                    return cmdf::edit_plugin(edit_target(row, m), PluginEdited{
                        .kind   = PluginEdited::Kind::SetToolEnabled,
                        .from   = PluginEdited::From::SettingsList,
                        .server = row.arg,
                        .tool   = row.arg2,
                        .flag   = !row.on,   // toggle
                    });
                }
                case se::Action::ApprovePlugin: {
                    // Enter on an untrusted project server = a deliberate
                    // "trust this server" — the row is explicitly labelled
                    // "trust & enable", so the keypress IS the consent (unlike
                    // hooks, which needs a y/N review). Approve just THIS
                    // server's spec (not the whole file), then reconnect so it
                    // spawns; a later-added server stays pending.
                    if (m.ui.plugins_loading)
                        return Cmd::none();
                    m.ui.plugins_loading = true;
                    return cmdf::edit_plugin(edit_target(row, m), PluginEdited{
                        .kind   = PluginEdited::Kind::Approve,
                        .from   = PluginEdited::From::SettingsList,
                        .server = row.arg,
                    });
                }
                case se::Action::ApproveHooks:
                    // Consent MUST be a deliberate terminal action — the
                    // picker can't safely own the y/N prompt while it holds
                    // the screen. Point at the CLI.
                    return set_status_toast(m,
                        "run `agentty hooks approve` in a shell to review "
                        "+ approve (consent is deliberate by design)");
                case se::Action::None:
                default:
                    return Cmd::none();
            }
        },
        [&](SettingsListRemove) -> Cmd {
            // `d` deletes the highlighted plugin — the deliberate, destructive
            // counterpart to Enter's reversible on/off toggle. Only applies
            // to a server row (Plugins concern, TogglePlugin action); ignored
            // on tool sub-rows and every other category.
            if (m.ui.plugins_loading) return Cmd::none();
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->concern != se::Category::Plugins)
                return Cmd::none();
            auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return Cmd::none();
            const se::Item& row = rows[static_cast<std::size_t>(o->index)];
            // A server row is either TogglePlugin (normal) or ApprovePlugin
            // (untrusted project) — both are removable with `d`. Tool sub-rows
            // and every other category are not.
            if ((row.action != se::Action::TogglePlugin
                 && row.action != se::Action::ApprovePlugin)
                || row.arg.empty())
                return Cmd::none();   // not a server row

            // Two-step: the first `d` on a row ARMS (stores the name); the
            // view then paints "press d again to remove". Only a second `d` on
            // the SAME row commits — so a stray keystroke never deletes a
            // hand-tuned mcp.json entry with no undo.
            if (o->confirm_remove != row.arg) {
                o->confirm_remove = row.arg;
                return Cmd::none();
            }
            o->confirm_remove.clear();

            m.ui.plugins_loading = true;
            return cmdf::edit_plugin(edit_target(row, m), PluginEdited{
                .kind   = PluginEdited::Kind::Remove,
                .from   = PluginEdited::From::SettingsList,
                .server = row.arg,
            });
        },
        // The outcome of a write requested from this list (cmd::edit_plugin).
        // Everything that used to follow the write inline lives here now:
        // reload the plugin model, toast, keep the cursor in range. An edit
        // requested from the EDIT PANE is that reducer's to handle.
        [&](PluginEdited& e) -> Cmd {
            // A leaf belongs to exactly one domain, so this list owns every
            // PluginEdited — including the ones the edit pane asked for. Those
            // are routed to the edit pane's handler explicitly, rather than
            // giving the reply two owners.
            if (e.from == PluginEdited::From::EditPane)
                return plugin_edit_result(m, std::move(e));
            using K = PluginEdited::Kind;
            if (!e.ok) {
                m.ui.plugins_loading = false;   // nothing is reconnecting
                switch (e.kind) {
                case K::Approve:
                    return set_status_toast(m,
                        "could not record approval (no project mcp.json?)");
                case K::Remove:
                    return set_status_toast(m, "could not remove '" + e.server + "'");
                case K::SetToolEnabled:
                    return set_status_toast(m, "could not toggle '" + e.tool + "'");
                default:
                    return set_status_toast(m, "could not toggle '" + e.server + "'");
                }
            }
            // A tool toggle only changed the exclude filter: no respawn, but
            // the snapshot must reflect the new enabled set, so re-snapshot
            // the live pool (reconnect=false). Every server-level change does
            // reconnect, since it changes which processes run.
            const bool reconnect = (e.kind != K::SetToolEnabled);
            std::string toast;
            switch (e.kind) {
            case K::SetServerDisabled:
                toast = (e.flag ? "disabled plugin '" : "enabled plugin '")
                      + e.server + "'";
                break;
            case K::SetToolEnabled:
                toast = (e.flag ? "enabled tool '" : "disabled tool '")
                      + e.tool + "' on " + e.server;
                break;
            case K::Approve:
                toast = "trusted project config — connecting…";
                break;
            case K::Remove:
                toast = "removed plugin '" + e.server + "'";
                break;
            default: break;
            }
            // Keep the cursor on a real row after the list rebuilds (a
            // removal shortens it).
            if (auto* oo = m.ui.panel.get<pn::SettingsList>()) {
                const int n = static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, n - 1));
            }
            return Cmd::batch(cmdf::load_plugins_async(reconnect),
                              set_status_toast(m, std::move(toast)));
        },
        [&](SettingsListEditOpen& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->input_active) return Cmd::none();
            if (o->concern != se::Category::Plugins) {
                // Other concerns have no editor form; `a` degrades to the
                // one-line starter prompt they already use, `e` is a no-op.
                if (e.add)
                    return reenter(m, Msg{SettingsListAddStart{}});
                return Cmd::none();
            }
            if (e.add) {
                // Add form: descend keeps this list as the Esc target.
                return reenter(m, Msg{OpenPluginEdit{{}, false}});
            }
            // Detail form for the highlighted server. Tool rows carry the
            // server in `arg` too, so `e` works from anywhere in a subtree.
            const auto rows = se::items_for(m, o->concern);
            if (o->index < 0 || o->index >= static_cast<int>(rows.size()))
                return Cmd::none();
            const auto& row = rows[static_cast<std::size_t>(o->index)];
            if (row.arg.empty()) return Cmd::none();   // header rows
            return reenter(m, Msg{OpenPluginEdit{row.arg, false}});
        },
        [&](SettingsListAddStart) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return Cmd::none();
            // Add is only meaningful for the file/config-backed concerns.
            if (o->concern != se::Category::Plugins
                && o->concern != se::Category::Commands
                && o->concern != se::Category::Agents)
                return Cmd::none();
            o->input_active = true;
            o->input.clear();
            o->cursor = 0;
            return Cmd::none();
        },
        [&](SettingsListChar& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return Cmd::none();
            const std::string utf8 = ui::utf8_encode(e.ch);
            o->input.insert(static_cast<std::size_t>(o->cursor), utf8);
            o->cursor += static_cast<int>(utf8.size());
            return Cmd::none();
        },
        [&](SettingsListPaste& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return Cmd::none();
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
            return Cmd::none();
        },
        [&](SettingsSearchInput& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->concern != se::Category::General) return Cmd::none();
            if (o->query.empty() && e.ch == U' ') return Cmd::none();   // no leading space
            o->query += ui::utf8_encode(e.ch);
            o->index = 0;                  // best match first
            m.ui.settings_list_scroll.y = 0;
            return Cmd::none();
        },
        [&](SettingsSearchClear) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return Cmd::none();
            o->query.clear();
            o->index = first_actionable(se::items_for(m, o->concern), 0, +1);
            m.ui.settings_list_scroll.y = 0;
            return Cmd::none();
        },
        [&](SettingsSearchBackspace) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || o->query.empty()) return Cmd::none();
            o->query.erase(static_cast<std::size_t>(
                ui::utf8_prev(o->query, static_cast<int>(o->query.size()))));
            // Back to the hub rows once the search is blank: land on the
            // first one you can act on.
            if (!se::is_search(*o)) o->query.clear();
            o->index = o->query.empty()
                ? first_actionable(se::items_for(m, o->concern), 0, +1) : 0;
            m.ui.settings_list_scroll.y = 0;
            return Cmd::none();
        },
        [&](SettingsListBackspace) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active || o->cursor <= 0)
                return Cmd::none();
            int p = ui::utf8_prev(o->input, o->cursor);
            o->input.erase(static_cast<std::size_t>(p),
                           static_cast<std::size_t>(o->cursor - p));
            o->cursor = p;
            return Cmd::none();
        },
        [&](SettingsListCancelInput) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o) return Cmd::none();
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            return Cmd::none();
        },
        [&](SettingsListSubmitInput) -> Cmd {
            auto* o = m.ui.panel.get<pn::SettingsList>();
            if (!o || !o->input_active) return Cmd::none();
            const std::string line = o->input;
            const se::Category concern = o->concern;
            o->input_active = false;
            o->input.clear();
            o->cursor = 0;
            if (line.empty()) return Cmd::none();   // empty = cancel
            // The create writes disk (mcp.json, or a starter file), so it is
            // an effect; SettingsAddDone below picks up its answer.
            return cmdf::settings_add(concern, line);
        },
        [&](SettingsAddDone& d) -> Cmd {
            std::string message = std::move(d.message);
            // Make it USABLE NOW, not after a restart. For Plugins the add
            // already wrote mcp.json; reload the live pool OFF the UI thread
            // (the connect handshake must never freeze the TUI — the bridge
            // bounds it with a deadline). A new command file reaches the
            // Model through a library reload. Agents are scanned per
            // task-tool call, so they are already live.
            Cmd reload = Cmd::none();
            if (d.ok && d.concern == se::Category::Plugins) {
                m.ui.plugins_loading = true;
                reload = cmdf::load_plugins_async(/*reconnect=*/true);
                message += " — connecting…";
            } else if (d.ok && d.concern == se::Category::Commands) {
                reload = cmdf::load_library();
            }
            // Re-clamp the (possibly grown) list to the top of the new row.
            if (auto* oo = m.ui.panel.get<pn::SettingsList>()) {
                const int cnt =
                    static_cast<int>(se::items_for(m, oo->concern).size());
                oo->index = std::clamp(oo->index, 0, std::max(0, cnt - 1));
            }
            return Cmd::batch(std::move(reload), set_status_toast(m, message));
        },
    }, sm);
}

} // namespace agentty::app::detail
