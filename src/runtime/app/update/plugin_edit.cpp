// plugin_edit.cpp — the plugin detail/add form's reducer.
//
// One pane, two modes (see pn::PluginEdit): detail (server non-empty) and
// add (server empty, kind-first). Everything flows through the shared form
// layer; this reducer owns only the plugin-specific consequences:
//   • kind change (add mode)  → rebuild the field set, preserving typed text
//   • enabled toggle (detail) → set_server_disabled + reload, live
//   • tool:<name> toggle      → set_tool_enabled + catalog invalidation, live
//   • Save                    → validate → add_server / update_server →
//                               reload → ascend with a toast
//   • Approve                 → approve_server (trust hash) → reload
//   • Remove                  → two-step arm (same contract as the list's `d`)
//
// The enabled/tool toggles apply IMMEDIATELY (they're reversible and the
// list behaves that way today); Save owns only the identity/transport
// fields, so a user who flips a toggle and never saves still gets what the
// pane showed them. The form's `dirty` flag tracks only save-owned rows.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <maya/core/overload.hpp>

#include "agentty/mcp/client.hpp"                    // plugin_model
#include "agentty/runtime/app/cmd_factory.hpp"       // load_plugins_async
#include "agentty/runtime/panel/plugin_form.hpp"
#include "agentty/tool/plugin.hpp"
#include "agentty/tool/registry.hpp"                 // invalidate_mcp_catalog

namespace pn = agentty::ui::panel;
namespace pf = agentty::plugin_form;
namespace fs = std::filesystem;
namespace cmdf = agentty::app::cmd;   // cmd_factory (locals named `cmd` shadow)

namespace agentty::app::detail {

using maya::overload;

namespace {

using pf::kNoteRemoveArmed;

// ── snapshot → form inputs ────────────────────────────────────────────

[[nodiscard]] std::string kind_of(const mcp::ServerState& s) {
    if (s.passthrough)     return pf::kKindPassthrough;
    if (!s.url.empty())    return pf::kKindHttp;   // sse renders as http; type
    return pf::kKindStdio;                         // is preserved on save
}

// Build detail-mode inputs for one server out of the Model's snapshot.
// Returns false when the server vanished (removed underneath the pane).
[[nodiscard]] bool inputs_for(const Model& m, const std::string& server,
                              pf::PluginFormInputs& in) {
    for (const auto& s : m.ui.plugins.servers) {
        if (s.name != server) continue;
        in.add_mode  = false;
        in.kind      = kind_of(s);
        in.name      = s.name;
        in.command   = s.command;
        in.url       = s.url;
        in.enabled   = !s.disabled;
        in.connected = s.connected;
        in.error     = s.error;
        in.scope_label = std::string{mcp::to_string(s.origin)};
        in.config_file = s.config_dir.empty()
            ? tools::plugin::config_path(false, m.env.user_root).string()
            : (fs::path{s.config_dir} / "mcp.json").string();
        in.untrusted = s.untrusted;
        for (const auto& t : s.tools)
            in.tool_rows.push_back({t.name, t.description, t.enabled});
        return true;
    }
    return false;
}

// ── form → values ───────────────────────────────────────────
// Field reads go through the form layer's SSOT accessors (form::text_of /
// toggle_of / choice_of) — no local variant-spelunking here.
using form::text_of;
using form::toggle_of;
using form::choice_of;

[[nodiscard]] std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        const auto cut = s.find_first_of(", ");
        std::string_view tok = s.substr(0, cut);
        if (!tok.empty()) out.emplace_back(tok);
        if (cut == std::string_view::npos) break;
        s.remove_prefix(cut + 1);
    }
    return out;
}

[[nodiscard]] bool valid_plugin_name(std::string_view n) {
    if (n.empty() || n.size() > 64 || n.front() == '-') return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'
              || c == '-' || c == ':'))
            return false;
    return true;
}

// Pull save-owned values out of the live form into a ServerSpec, validating
// as we go. On failure, stamps the offending row's `error` and returns
// false — the form renders the message inline, exactly where the fix goes.
[[nodiscard]] bool spec_from_form(form::Form& f, const std::string& kind,
                                  std::string name, tools::plugin::ServerSpec& spec) {
    auto fail = [&](const char* id, std::string msg) {
        if (auto* fld = f.find(id)) fld->error = std::move(msg);
        return false;
    };
    // Clear stale errors — a re-save must not show last attempt's ghosts.
    for (auto& fld : f.fields) fld.error.clear();

    if (name.empty()) name = text_of(f, pf::kName);
    if (!valid_plugin_name(name))
        return fail(pf::kName, "letters, digits, _ - : only");
    spec.name = std::move(name);

    if (kind == pf::kKindStdio) {
        spec.command = text_of(f, pf::kCommand);
        if (spec.command.empty())
            return fail(pf::kCommand, "a stdio server needs a command");
        spec.args = split_list(text_of(f, pf::kArgs));
        spec.type = "stdio";
    } else if (kind == pf::kKindPassthrough) {
        spec.url = text_of(f, pf::kUrl);
        if (spec.url.rfind("http://", 0) != 0
            && spec.url.rfind("https://", 0) != 0)
            return fail(pf::kUrl, "must start with http:// or https://");
        spec.type = "passthrough";
        spec.passthrough = split_list(text_of(f, pf::kTools));
        // Detail mode has no kTools row (tools are toggle rows); keep the
        // config's existing list by leaving spec.passthrough empty there —
        // update_server preserves an absent field.
        if (f.find(pf::kTools) && spec.passthrough.empty())
            return fail(pf::kTools, "at least one tool name");
    } else {   // http / sse
        spec.url = text_of(f, pf::kUrl);
        if (spec.url.rfind("http://", 0) != 0
            && spec.url.rfind("https://", 0) != 0)
            return fail(pf::kUrl, "must start with http:// or https://");
        spec.type = (kind == pf::kKindSse) ? "sse" : "http";
    }
    return true;
}

// Rebuild the ADD form for a new kind, carrying over what the user typed.
void rebuild_add_form(pn::PluginEdit& o, const std::string& kind) {
    pf::PluginFormInputs in;
    in.add_mode = true;
    in.kind     = kind;
    in.name     = text_of(o.form, pf::kName);
    in.command  = text_of(o.form, pf::kCommand);
    in.args     = text_of(o.form, pf::kArgs);
    in.url      = text_of(o.form, pf::kUrl);
    in.tools    = text_of(o.form, pf::kTools);
    in.advertise= toggle_of(o.form, pf::kAdvertise);
    in.project  = toggle_of(o.form, pf::kScopeProject);
    const int cursor = o.form.cursor;
    o.form = pf::build_form(in);
    o.form.cursor = std::min(cursor,
        std::max(0, static_cast<int>(o.form.fields.size()) - 1));
    o.built_kind = kind;
}

[[nodiscard]] fs::path config_target(const pn::PluginEdit& o, const Model& m) {
    if (o.server.empty())
        return tools::plugin::config_path(toggle_of(o.form, pf::kScopeProject), m.env.user_root);
    for (const auto& s : m.ui.plugins.servers)
        if (s.name == o.server && !s.config_dir.empty())
            return fs::path{s.config_dir} / "mcp.json";
    return tools::plugin::config_path(false, m.env.user_root);
}

} // namespace

Cmd plugin_edit_update(Model& m, msg::PluginEditMsg pm) {
    return std::visit(overload{
        [&](OpenPluginEdit& e) -> Cmd {
            pf::PluginFormInputs in;
            if (e.server.empty()) {
                in.add_mode = true;
                in.project  = e.project;
            } else if (!inputs_for(m, e.server, in)) {
                return set_status_toast(m, "plugin '" + e.server + "' not found");
            }
            pn::PluginEdit pane{{}, pf::build_form(in), e.server,
                                e.project, in.kind};
            m.ui.panel.descend(std::move(pane));
            return Cmd::none();
        },

        [&](ClosePluginEdit) -> Cmd {
            ascend(m);   // Esc: back to the pane that opened this, or close
            return Cmd::none();
        },

        [&](PluginEditPaste& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::PluginEdit>();
            if (o) form::paste_into(o->form, e.text);   // SSOT guard inside
            return Cmd::none();
        },

        [&](PluginEditKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::PluginEdit>();
            if (!o) return Cmd::none();

            const auto* row = o->form.focused();
            const std::string row_id = row ? row->id : std::string{};

            const auto applied = form::keys::apply(o->form, e.action);

            if (applied.close)
                return plugin_edit_update(m, msg::PluginEditMsg{ClosePluginEdit{}});

            // Moving off an armed Remove row disarms it — same contract as
            // the settings list's `d` (any navigation cancels a pending
            // destructive step; only Enter-again on the SAME row fires).
            if (o->form.note == kNoteRemoveArmed) {
                const auto* now_row = o->form.focused();
                if (!now_row || now_row->id != pf::kRemove) {
                    o->form.note.clear();
                    o->form.note_replaces_grammar = false;
                }
            }

            // ── kind change (add mode): rebuild the field set ─────────
            if (applied.changed && row_id == pf::kKind && o->server.empty()) {
                const std::string kind = choice_of(o->form, pf::kKind);
                if (kind != o->built_kind) rebuild_add_form(*o, kind);
                return Cmd::none();
            }

            // ── live toggles (detail mode) ────────────────────────────
            // Each write is an effect (cmd::edit_plugin). Its outcome comes
            // back as PluginEdited and is handled in plugin_edit_result().
            if (applied.changed && !o->server.empty()) {
                const fs::path path = config_target(*o, m);
                if (row_id == pf::kEnabled) {
                    const bool on = toggle_of(o->form, pf::kEnabled);
                    return cmdf::edit_plugin(path, PluginEdited{
                        .kind   = PluginEdited::Kind::SetServerDisabled,
                        .from   = PluginEdited::From::EditPane,
                        .server = o->server,
                        .flag   = !on,
                        .project = o->project,
                    });
                }
                if (row_id.starts_with(pf::kToolPrefix)) {
                    const std::string bare =
                        row_id.substr(std::string_view{pf::kToolPrefix}.size());
                    return cmdf::edit_plugin(path, PluginEdited{
                        .kind   = PluginEdited::Kind::SetToolEnabled,
                        .from   = PluginEdited::From::EditPane,
                        .server = o->server,
                        .tool   = bare,
                        .flag   = toggle_of(o->form, row_id.c_str()),
                        .project = o->project,
                    });
                }
                // Any other change (url text etc.) is save-owned; fall out.
            }

            // Leaving an edited field IS the save (Applied::left_field) —
            // there is no ^S. Detail mode only: an add form is still under
            // construction, so committing a name with no URL yet would
            // just stamp errors on rows the user hasn't reached; its
            // explicit "Add plugin" action row does the write. Field-exit
            // commits keep the pane OPEN (leaving a field is not leaving
            // the form); the action row closes.
            const bool exit_commit = applied.left_field && !o->server.empty();
            const bool want_save = exit_commit
                || (applied.fired && row_id == pf::kSave);

            if (!applied.fired && !want_save) return Cmd::none();

            // ── action rows ───────────────────────────────────────────
            if (row_id == pf::kApprove && !o->server.empty()) {
                return cmdf::edit_plugin(config_target(*o, m), PluginEdited{
                    .kind   = PluginEdited::Kind::Approve,
                    .from   = PluginEdited::From::EditPane,
                    .server = o->server,
                    .project = o->project,
                });
            }

            if (row_id == pf::kRemove && !o->server.empty() && !want_save) {
                // Two-step: first Enter arms (note explains), second fires.
                if (o->form.note != kNoteRemoveArmed) {
                    o->form.note = kNoteRemoveArmed;
                    o->form.note_replaces_grammar = true;
                    return Cmd::none();
                }
                return cmdf::edit_plugin(config_target(*o, m), PluginEdited{
                    .kind   = PluginEdited::Kind::Remove,
                    .from   = PluginEdited::From::EditPane,
                    .server = o->server,
                    .project = o->project,
                });
            }

            if (want_save) {
                const std::string kind = o->server.empty()
                    ? choice_of(o->form, pf::kKind) : o->built_kind;
                tools::plugin::ServerSpec spec;
                if (!spec_from_form(o->form, kind, o->server, spec))
                    return Cmd::none();   // inline errors set
                const bool add = o->server.empty();
                // `server` names the entry the reply is about: the new name
                // for an add, the existing one for an update.
                PluginEdited req{
                    .kind        = add ? PluginEdited::Kind::Add
                                       : PluginEdited::Kind::Update,
                    .from        = PluginEdited::From::EditPane,
                    .server      = spec.name,
                    .exit_commit = exit_commit,
                    .project     = o->project,
                };
                return cmdf::edit_plugin(config_target(*o, m), std::move(req),
                                         std::move(spec));
            }

            return Cmd::none();
        },
    }, pm);
}

// The outcome of a write the edit pane requested (cmd::edit_plugin), routed
// here by settings_list's PluginEdited arm. Everything that used to follow
// each write inline: reload the plugin model, toast, close or keep the pane,
// and the two inline field errors an add/update can produce.
//
// THE PANE MAY HAVE MOVED ON. The reply lands one message after the request,
// and by then the user may have closed the pane or opened it on another
// server. So anything that touches the pane first checks it is still open on
// the server this reply is about; a stale reply still reloads the plugin
// model and toasts (the write DID happen), it just doesn't edit a form that
// is no longer the one it came from.
Cmd plugin_edit_result(Model& m, PluginEdited e) {
    using K = PluginEdited::Kind;
    auto* o = m.ui.panel.get<pn::PluginEdit>();
    // Same pane, same entry. For an add the pane was open on server "" and
    // the reply names the new server, so an add matches an open add-form.
    const bool same_pane = o && o->project == e.project
        && (o->server == e.server || (e.kind == K::Add && o->server.empty()));

    if (!e.ok) {
        switch (e.kind) {
        case K::SetServerDisabled:
            return set_status_toast(m, "could not write mcp.json");
        case K::SetToolEnabled:
            return set_status_toast(m, "could not toggle '" + e.tool + "'");
        case K::Approve:
            return set_status_toast(m, "approve failed");
        case K::Remove:
            return set_status_toast(m, "remove failed");
        case K::Add:
        case K::Update:
            if (same_pane) {
                if (e.already_exists) {
                    if (auto* fld = o->form.find(pf::kName))
                        fld->error = "'" + e.server + "' already exists";
                } else {
                    o->form.note = "write failed — is the file valid JSON?";
                }
                return Cmd::none();
            }
            return set_status_toast(m, "could not save '" + e.server + "'");
        }
        return Cmd::none();
    }

    switch (e.kind) {
    case K::SetServerDisabled:
        return Cmd::batch(cmdf::load_plugins_async(/*reconnect=*/true),
                          set_status_toast(m, e.flag ? "disabled" : "enabled"));
    case K::SetToolEnabled:
        // The catalog was invalidated on the worker, beside the write.
        return Cmd::batch(cmdf::load_plugins_async(/*reconnect=*/false),
                          set_status_toast(m, (e.flag ? "enabled '" : "disabled '")
                                              + e.tool + "'"));
    case K::Approve:
        return Cmd::batch(
            cmdf::load_plugins_async(/*reconnect=*/true),
            set_status_toast(m, "approved '" + e.server + "'"),
            // Reopen once the reload lands so the pane reflects the
            // post-approval state (trusted → connecting). Only if the user is
            // still looking at it — reopening a pane they closed would yank
            // them back to it.
            same_pane ? Cmd::after(std::chrono::milliseconds{50},
                                   Msg{OpenPluginEdit{e.server, e.project}})
                      : Cmd::none());
    case K::Remove:
        if (same_pane) ascend(m);
        return Cmd::batch(cmdf::load_plugins_async(/*reconnect=*/true),
                          set_status_toast(m, "removed '" + e.server + "'"));
    case K::Add:
    case K::Update: {
        if (same_pane) o->form.dirty = false;   // committed: footer drops "unsaved"
        if (e.exit_commit) {
            // Field-exit commit: written + applied, pane stays open. The
            // catalog reload runs so the new value is live immediately.
            return Cmd::batch(cmdf::load_plugins_async(/*reconnect=*/true),
                              set_status_toast(m, "saved '" + e.server + "'"));
        }
        if (same_pane) ascend(m);
        return Cmd::batch(
            cmdf::load_plugins_async(/*reconnect=*/true),
            set_status_toast(m, (e.kind == K::Add ? "added '" : "saved '")
                                + e.server + "'"));
    }
    }
    return Cmd::none();
}

} // namespace agentty::app::detail
