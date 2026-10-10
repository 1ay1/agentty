// web_search.cpp — Settings → Web Search: the web_search tool's policy.
//
// Open descends from the settings list (Esc walks back), keys go through the
// shared form layer, and a committed edit writes m.d.persisted.web_search,
// returns the save, and rebuilds the rows from the Model so the pane can
// never show a value it did not save. Telling the TOOL LAYER is not this
// file's job: publish_derived sees the record move and returns
// PublishWebSearchPolicy (update.cpp), the same way Smart Mode's config reaches
// the subagent router.
//
// WHEN an edit commits is the form layer's answer, not this file's:
//   * toggles and ←/→ on a number   — on the keystroke (`changed`, browsing)
//   * typed numbers and text        — on leaving the field (`left_field`)
// Committing a text row per keystroke would rebuild the form under the caret
// and save "p", "pi", "pin", … to settings.json on the way to "pinterest.com".

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include <mcp/tools/web_search.hpp>

#include "agentty/runtime/panel/web_search_form.hpp"
#include "agentty/tool/web_search_secret.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

namespace {

// Put the cursor back where it was after a rebuild, but never on a header
// row (add_rows emits a "Search" header first) and never past the end. A
// header under the cursor reads as a frozen pane: nothing on it reacts.
void restore_cursor(form::Form& f, int cursor) {
    const int n = static_cast<int>(f.fields.size());
    if (n == 0) { f.cursor = 0; return; }
    f.cursor = std::clamp(cursor, 0, n - 1);
    if (f.fields[static_cast<std::size_t>(f.cursor)].is_header())
        form::move_edge(f, /*last=*/false);
}

// What the pane may learn about a key: that it exists and where from. The
// value never comes back out of the store into the Model.
[[nodiscard]] web_search_form::KeyOrigin key_origin() {
    return [](std::string_view service) {
        return tools::web_search_secret::origin(service);
    };
}

[[nodiscard]] form::Form build(const web_search_cfg::Config& c) {
    return web_search_form::build_form(c, key_origin());
}

// A key typed into a Secret row goes to the key store, not to the Model and
// not to settings.json (same rule as the embeddings key, rag.cpp). The row is
// emptied by the rebuild that follows, so the text does not outlive the edit
// on screen either. Returns the toast to show, or "" when no key was entered.
[[nodiscard]] std::string take_typed_keys(form::Form& f) {
    std::string note;
    for (auto& row : f.fields) {
        const auto service = web_search_form::key_row_service(row.id);
        if (service.empty() || row.locked) continue;
        auto* sec = std::get_if<form::field::Secret>(&row.value);
        if (!sec || sec->value.empty()) continue;
        // Trim what a paste drags along: a trailing newline, a "Bearer " the
        // vendor's docs show in front of the key.
        std::string key = sec->value;
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(0, 1);
        if (key.rfind("Bearer ", 0) == 0) key.erase(0, 7);
        const auto* svc = ::mcp::tools::find_web_search_service(service);
        const std::string name = svc ? svc->label : std::string{service};
        if (!tools::web_search_secret::plausible(key))
            note = name + ": that does not look like an API key (8+ characters, no spaces)";
        else if (!tools::web_search_secret::store(service, key))
            note = name + ": key not saved \xe2\x80\x94 no secure store available";
        else
            note = name + ": key saved";
        // Overwrite before dropping: the typed text should not linger in freed
        // memory any longer than the edit itself.
        std::fill(sec->value.begin(), sec->value.end(), '\0');
        sec->value.clear();
        sec->cursor = 0;
    }
    return note;
}

// Persist `cfg` and rebuild the pane around it, cursor kept (clamped: a
// rebuild may lock or unlock rows, never add or remove them today, but the
// cursor must not be able to land past the end if that changes).
[[nodiscard]] Cmd commit(Model& m, pn::WebSearch& o, web_search_cfg::Config cfg) {
    const auto was = web_search_cfg::mode(m.d.persisted.web_search);
    m.d.persisted.web_search = cfg;
    Cmd out = save_record(m);

    const int cursor = o.form.cursor;
    o.form = build(m.d.persisted.web_search);
    restore_cursor(o.form, cursor);

    // The one change worth a toast: the mode itself. Auto is the quiet
    // default; naming it keeps "why are the counts locked?" answerable.
    const auto now = web_search_cfg::mode(cfg);
    if (now != was) {
        const char* text = now == web_search_cfg::Mode::On   ? "Web search on"
                         : now == web_search_cfg::Mode::Off  ? "Web search off"
                                                         : "Web search auto";
        out = Cmd::batch(std::move(out), set_status_toast(m, text));
    }
    return out;
}

} // namespace

Cmd web_search_update(Model& m, msg::WebSearchMsg wm) {
    return std::visit(overload{
        [&](OpenWebSearch) -> Cmd {
            pn::WebSearch o;
            o.form = build(m.d.persisted.web_search);
            // Open on the master switch, not on the group header above it.
            restore_cursor(o.form, 0);
            m.ui.panel.descend(std::move(o));
            return Cmd::none();
        },
        [&](CloseWebSearch) -> Cmd {
            ascend(m);
            return Cmd::none();
        },
        [&](WebSearchPaste& e) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::WebSearch>())
                (void)form::paste_into(o->form, e.text);   // SSOT: guard + dirty
            return Cmd::none();
        },
        [&](WebSearchKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::WebSearch>();
            if (!o) return Cmd::none();

            const auto applied = form::keys::apply(o->form, e.action);
            if (applied.close)
                return web_search_update(m, msg::WebSearchMsg{CloseWebSearch{}});

            // "Remove key" is an Action row; Enter on it fires it.
            if (applied.fired) {
                if (const auto* row = o->form.focused()) {
                    const auto service = web_search_form::key_clear_service(row->id);
                    if (!service.empty()) {
                        (void)tools::web_search_secret::erase(service);
                        const auto* svc = ::mcp::tools::find_web_search_service(service);
                        const int cursor = o->form.cursor;
                        o->form = build(m.d.persisted.web_search);
                        restore_cursor(o->form, cursor);
                        return set_status_toast(m, (svc ? svc->label : std::string{service})
                                                   + ": key removed");
                    }
                }
                return Cmd::none();
            }

            const bool settled = applied.left_field
                              || (applied.changed && !o->form.editing());
            // Mid-edit keystrokes (Insert, Backspace, Delete, caret moves)
            // report `changed` while the field is still EDITING. They must
            // return here and leave the form alone: rebuilding would reset
            // the row to the saved value and drop Editing focus, which
            // erased every keystroke after the first — a number row took
            // one digit, and Backspace/Delete in a text row appeared to do
            // nothing because the deleted character was restored at once.
            // web_search_pane_test pins this.
            if (!settled) return Cmd::none();

            // A typed API key is stored (and the row emptied) before anything
            // else reads the form. The key store is not part of the Model, so
            // this is not a settings change and is not compared below.
            const std::string key_note = take_typed_keys(o->form);

            web_search_cfg::Config cfg = m.d.persisted.web_search;
            web_search_form::apply_form(o->form, cfg);
            // Settled on the value already on record (typed and put back, a
            // toggle flipped twice): nothing to save. Rebuild anyway so the
            // row shows what the registry accepted (clamped, cleaned) and
            // its provenance column is current.
            if (cfg == m.d.persisted.web_search) {
                const int cursor = o->form.cursor;
                o->form = build(m.d.persisted.web_search);
                restore_cursor(o->form, cursor);
                if (key_note.empty()) return Cmd::none();
                return set_status_toast(m, key_note);
            }
            Cmd saved = commit(m, *o, std::move(cfg));
            if (key_note.empty()) return saved;
            return Cmd::batch(std::move(saved), set_status_toast(m, key_note));
        },
    }, wm);
}

} // namespace agentty::app::detail
