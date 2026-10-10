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
#include <utility>
#include <variant>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/web_search_form.hpp"

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

// Persist `cfg` and rebuild the pane around it, cursor kept (clamped: a
// rebuild may lock or unlock rows, never add or remove them today, but the
// cursor must not be able to land past the end if that changes).
[[nodiscard]] Cmd commit(Model& m, pn::WebSearch& o, web_search_cfg::Config cfg) {
    const auto was = web_search_cfg::mode(m.d.persisted.web_search);
    m.d.persisted.web_search = cfg;
    Cmd out = save_record(m);

    const int cursor = o.form.cursor;
    o.form = web_search_form::build_form(m.d.persisted.web_search);
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
            o.form = web_search_form::build_form(m.d.persisted.web_search);
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

            web_search_cfg::Config cfg = m.d.persisted.web_search;
            web_search_form::apply_form(o->form, cfg);
            // Settled on the value already on record (typed and put back, a
            // toggle flipped twice): nothing to save. Rebuild anyway so the
            // row shows what the registry accepted (clamped, cleaned) and
            // its provenance column is current.
            if (cfg == m.d.persisted.web_search) {
                const int cursor = o->form.cursor;
                o->form = web_search_form::build_form(m.d.persisted.web_search);
                restore_cursor(o->form, cursor);
                return Cmd::none();
            }
            return commit(m, *o, std::move(cfg));
        },
    }, wm);
}

} // namespace agentty::app::detail
