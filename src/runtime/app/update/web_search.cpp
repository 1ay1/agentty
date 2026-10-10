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
//
// API keys are the one thing here that is not Model state. The key store is
// IO (a sealed file, and a `secret-tool` / Keychain call when the OS keystore
// is on), so this reducer never touches it: reading which keys exist, storing
// a typed key and removing one each run as a task on a worker
// (Cmd::task_isolated), and the outcome comes back as WebSearchKeysRead /
// WebSearchKeySaved / WebSearchKeyRemoved. Same shape as the embeddings key in
// the Retrieval pane. No message, toast or Model field ever carries a key; the
// typed text lives only in the Secret row until the field is left, and in the
// task that stores it.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <maya/core/overload.hpp>

#include <mcp/tools/web_search.hpp>

#include "agentty/runtime/panel/web_search_form.hpp"
#include "agentty/tool/web_search_secret.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

namespace {

// Put the cursor back where it was after a rebuild: on the SAME ROW (by id)
// when it still exists, else at the same index, never on a header row and
// never past the end. A header under the cursor reads as a frozen pane:
// nothing on it reacts.
//
// By id because a rebuild can now add and remove rows: choosing a service for
// a slot replaces the dial and key rows under it, and storing a key adds a
// "Remove key" row. Keeping only the index would leave the cursor on whatever
// row slid into that position — after picking Exa for Primary, on Exa's
// "Mode" row instead of on Primary.
void restore_cursor(form::Form& f, int cursor, std::string_view id = {}) {
    const int n = static_cast<int>(f.fields.size());
    if (n == 0) { f.cursor = 0; return; }
    if (!id.empty())
        for (int i = 0; i < n; ++i)
            if (f.fields[static_cast<std::size_t>(i)].id == id) { f.cursor = i; return; }
    f.cursor = std::clamp(cursor, 0, n - 1);
    if (f.fields[static_cast<std::size_t>(f.cursor)].is_header())
        form::move_edge(f, /*last=*/false);
}

// The id of the focused row, copied (the form is about to be replaced).
[[nodiscard]] std::string focused_id(const form::Form& f) {
    const auto* row = f.focused();
    return row ? row->id : std::string{};
}

// The pane's view of where each key comes from, as last read off the UI
// thread. Until the first read answers, every key row says so.
[[nodiscard]] web_search_form::KeyOrigin key_origin(const pn::WebSearch& o) {
    if (!o.keys_read)
        return [](std::string_view) { return std::string{"checking\xe2\x80\xa6"}; };
    return [&o](std::string_view service) {
        for (const auto& [id, origin] : o.key_origins)
            if (id == service) return origin;
        return std::string{};
    };
}

// Rebuild the rows from the Model, the cursor kept on the same row (a rebuild
// can add and remove rows, see restore_cursor).
void rebuild(const Model& m, pn::WebSearch& o, std::string_view prefer_id = {}) {
    o.rows_stale = false;
    const int cursor = o.form.cursor;
    const std::string id = prefer_id.empty() ? focused_id(o.form) : std::string{prefer_id};
    o.form = web_search_form::build_form(m.d.persisted.web_search, key_origin(o));
    restore_cursor(o.form, cursor, id);
}

// Ask a worker which services have a key, and from where. Every keyed service
// in the catalogue, not only the ones in a slot: choosing a service for a slot
// must not need another read.
//
// Each read takes a new generation, so of two reads in flight only the newer
// one's answer is shown.
[[nodiscard]] Cmd read_key_origins(pn::WebSearch& o) {
    ++o.keys_gen;
    std::vector<std::string> services;
    for (const auto& svc : ::mcp::tools::web_search_services())
        if (svc.needs_key) services.push_back(svc.id);
    return Cmd::task_isolated(
        [](jaal::Sink<Msg> out, std::stop_token, std::uint64_t gen,
           std::vector<std::string> ids) {
            WebSearchKeysRead r;
            r.gen = gen;
            for (auto& id : ids) {
                std::string origin = tools::web_search_secret::origin(id);
                r.origins.emplace_back(std::move(id), std::move(origin));
            }
            out.send(Msg{msg::WebSearchMsg{std::move(r)}});
        },
        o.keys_gen, std::move(services));
}

// Trim what a paste drags along: surrounding whitespace, and a "Bearer " the
// vendors' docs print in front of the key.
[[nodiscard]] std::string clean_typed_key(std::string key) {
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(0, 1);
    if (key.rfind("Bearer ", 0) == 0) key.erase(0, 7);
    return key;
}

[[nodiscard]] std::string label_of(std::string_view service) {
    const auto* svc = ::mcp::tools::find_web_search_service(service);
    return svc ? svc->label : std::string{service};
}

// A key typed into a Secret row leaves the form here: the row is emptied (and
// the text overwritten first, so it does not linger in freed memory), and the
// key goes to a worker that stores it -- never to the Model or settings.json.
// Returns the store task (or a toast for a key that is plainly not a key), or
// none when no key was typed.
[[nodiscard]] Cmd take_typed_keys(Model& m, pn::WebSearch& o) {
    std::vector<Cmd> out;
    for (auto& row : o.form.fields) {
        const std::string service{web_search_form::key_row_service(row.id)};
        if (service.empty() || row.locked) continue;
        auto* sec = std::get_if<form::field::Secret>(&row.value);
        if (!sec || sec->value.empty()) continue;
        std::string key = clean_typed_key(sec->value);
        std::fill(sec->value.begin(), sec->value.end(), '\0');
        sec->value.clear();
        sec->cursor = 0;
        if (!tools::web_search_secret::plausible(key)) {
            std::fill(key.begin(), key.end(), '\0');
            out.push_back(set_status_toast(m, label_of(service)
                + ": that does not look like an API key (8+ characters, no spaces)"));
            continue;
        }
        ++o.keys_gen;   // a read already in flight must not undo this
        out.push_back(Cmd::task_isolated(
            [](jaal::Sink<Msg> sink, std::stop_token, std::string svc, std::string k) {
                const bool ok = tools::web_search_secret::store(svc, k);
                std::fill(k.begin(), k.end(), '\0');
                sink.send(Msg{msg::WebSearchMsg{WebSearchKeySaved{std::move(svc), ok}}});
            },
            service, std::move(key)));
    }
    if (out.empty()) return Cmd::none();
    if (out.size() == 1) return std::move(out.front());
    return Cmd::batch(std::move(out));
}

// Persist `cfg` and rebuild the pane around it, the cursor kept on the same
// row (a rebuild can add and remove rows, see restore_cursor).
[[nodiscard]] Cmd commit(Model& m, pn::WebSearch& o, web_search_cfg::Config cfg) {
    const auto was = web_search_cfg::mode(m.d.persisted.web_search);
    m.d.persisted.web_search = cfg;
    Cmd out = save_record(m);

    rebuild(m, o);

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
            // Built synchronously (a pane must never own the keyboard with no
            // form to act on); the key rows say "checking…" until the read
            // below answers.
            o.form = web_search_form::build_form(m.d.persisted.web_search, key_origin(o));
            // Open on the master switch, not on the group header above it.
            restore_cursor(o.form, 0);
            m.ui.panel.descend(std::move(o));
            return read_key_origins(*m.ui.panel.get<pn::WebSearch>());
        },
        [&](WebSearchKeysRead& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::WebSearch>();
            // Closed since, or overtaken by a store/removal: a newer read is
            // on its way.
            if (!o || e.gen != o->keys_gen) return Cmd::none();
            o->key_origins = std::move(e.origins);
            o->keys_read   = true;
            // Mid-edit, a rebuild would throw away what is being typed: mark
            // the rows stale and rebuild the moment the edit or dropdown ends.
            if (o->form.editing() || o->form.choosing()) o->rows_stale = true;
            else rebuild(m, *o);
            return Cmd::none();
        },
        [&](WebSearchKeySaved& e) -> Cmd {
            Cmd toast = set_status_toast(m, label_of(e.service) + (e.ok
                ? ": key saved"
                : ": key not saved \xe2\x80\x94 no secure store available"));
            auto* o = m.ui.panel.get<pn::WebSearch>();
            if (!o) return toast;
            return Cmd::batch(std::move(toast), read_key_origins(*o));
        },
        [&](WebSearchKeyRemoved& e) -> Cmd {
            Cmd toast = set_status_toast(m, label_of(e.service) + (e.ok
                ? ": key removed"
                : ": key NOT removed \xe2\x80\x94 the keystore refused; it is still stored"));
            auto* o = m.ui.panel.get<pn::WebSearch>();
            if (!o) return toast;
            return Cmd::batch(std::move(toast), read_key_origins(*o));
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
                    // A COPY: key_clear_service returns a view into the row's
                    // id, and the row dies with the form rebuilt below.
                    std::string service{web_search_form::key_clear_service(row->id)};
                    if (!service.empty()) {
                        // Show the result at once (the row goes, the key row
                        // says "not set") and land on the key row; the
                        // removal itself, and the re-read that confirms it,
                        // run on a worker.
                        ++o->keys_gen;
                        bool found = false;
                        for (auto& [id, origin] : o->key_origins)
                            if (id == service) { origin.clear(); found = true; }
                        if (!found) o->key_origins.emplace_back(service, std::string{});
                        rebuild(m, *o, std::string{web_search_form::kKeyPrefix} + service);
                        return Cmd::task_isolated(
                            [](jaal::Sink<Msg> out, std::stop_token, std::string svc) {
                                (void)tools::web_search_secret::erase(svc);
                                // Judged by what is left, not by erase()'s
                                // return: "nothing was there" is a success.
                                const bool ok =
                                    tools::web_search_secret::origin(svc) != "stored";
                                out.send(Msg{msg::WebSearchMsg{
                                    WebSearchKeyRemoved{std::move(svc), ok}}});
                            },
                            std::move(service));
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
            if (!settled) {
                // Key origins arrived mid-edit: show them now that the edit
                // or dropdown is over (Esc out of an unchanged field settles
                // nothing, so this is the only place that would notice).
                if (o->rows_stale && !o->form.editing() && !o->form.choosing()) {
                    o->rows_stale = false;
                    rebuild(m, *o);
                }
                return Cmd::none();
            }

            // A typed API key leaves the form (and goes to a worker) before
            // anything else reads it. The key store is not part of the Model,
            // so this is not a settings change and is not compared below.
            Cmd keys = take_typed_keys(m, *o);

            web_search_cfg::Config cfg = m.d.persisted.web_search;
            web_search_form::apply_form(o->form, cfg);
            // Settled on the value already on record (typed and put back, a
            // toggle flipped twice): nothing to save. Rebuild anyway so the
            // row shows what the registry accepted (clamped, cleaned) and
            // its provenance column is current.
            if (cfg == m.d.persisted.web_search) {
                rebuild(m, *o);
                return keys;
            }
            return Cmd::batch(commit(m, *o, std::move(cfg)), std::move(keys));
        },
    }, wm);
}

} // namespace agentty::app::detail
