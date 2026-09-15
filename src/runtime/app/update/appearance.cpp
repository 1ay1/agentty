// appearance_update — reducer for the Appearance pane.
//
// How agentty looks: theme, colour tier, background polarity, density,
// motion, chrome. One pane, one rule — EVERY row is live.
//
// ── Why there is no apply step ───────────────────────────────────────────
// A theme is judged by LOOKING at it. If choosing and seeing are separated
// by a save keystroke (never mind a restart) the only way to evaluate a
// scheme is to commit to it first, which is exactly backwards. So each row
// writes through on the keystroke that changes it: mutate `m.d.ui`, persist
// to the user store, rebuild the form, and the next frame is painted with
// it. `Form::dirty` is never set here, because it can never be true.
//
// ── Why the theme browser floats over the pane ───────────────────────────
// It does NOT replace it. The pane stays painted underneath, and moving the
// highlight applies the scheme immediately — so the list is its own preview
// and you are judging the theme on the real UI rather than on swatches.
// Esc restores whatever you came in with; Enter keeps what you are seeing.
//
// ── Why appearance is a USER setting ─────────────────────────────────────
// A light terminal is a property of your eyes, not of the repo. These go to
// the user store and follow you between checkouts, never to a project
// .agentty/settings.json.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/panel/form_keys.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
namespace up = agentty::ui_prefs;

namespace {

// Persist the prefs to the USER store. Write-behind at the Deps seam, so a
// reducer never stalls a frame on the disk — which is what makes saving on
// every keystroke affordable in the first place.
void persist(const Model& m) {
    auto s = deps().load_settings();
    s.ui = m.d.ui;
    deps().save_settings(s);
}

// Rebuild the pane's rows from the prefs. The form is a PROJECTION of
// `m.d.ui`, never a second copy of it: the reducer edits the prefs and
// re-derives, so there is no path on which the rows and the truth disagree.
// Cursor and focus are carried across because the user's place in the list
// is not part of what changed.
void reproject(Model& m) {
    auto* o = m.ui.panel.get<pn::Appearance>();
    if (!o) return;
    const int cursor = o->pane.form.cursor;
    auto focus = o->pane.form.focus;
    o->pane.form = pn::build_appearance_form(m.d.ui, /*tty=*/true);
    o->pane.form.cursor = std::clamp(cursor, 0,
                                std::max(0, static_cast<int>(o->pane.form.fields.size()) - 1));
    o->pane.form.focus = focus;
}

// Apply a row's current form value back onto the prefs.
//
// Keyed by row id through a total switch over the ids the builder emits: a
// positional table would have to be kept in the builder's order by hand, and
// an id that no row carries is a silently dead setting the compiler cannot
// see. (Hence the named kAp* constants on both sides.)
void pull_field(Model& m, const form::Field& f) {
    const auto idx = [&]() -> int {
        const auto* c = std::get_if<form::field::Choice>(&f.value);
        return c ? c->normalized(c->index) : 0;
    };
    const auto on = [&]() -> bool {
        const auto* t = std::get_if<form::field::Toggle>(&f.value);
        return t && t->on;
    };

    up::Prefs& p = m.d.ui;
    if (f.id == pn::kApTier)            p.tier        = static_cast<up::ColorTier>(idx());
    else if (f.id == pn::kApPolarity)   p.polarity    = static_cast<up::Polarity>(idx());
    else if (f.id == pn::kApDensity)    p.density     = static_cast<up::Density>(idx());
    else if (f.id == pn::kApMotion)     p.motion      = static_cast<up::Motion>(idx());
    else if (f.id == pn::kApToolOutput) p.tool_output = static_cast<up::ToolOutput>(idx());
    else if (f.id == pn::kApThinking)   p.thinking    = static_cast<up::Thinking>(idx());
    else if (f.id == pn::kApTimestamps) p.timestamps  = static_cast<up::Timestamps>(idx());
    else if (f.id == pn::kApSyntax)     p.syntax        = on();
    else if (f.id == pn::kApCompact)    p.compact_turns = on();
    else if (f.id == pn::kApProseWidth) {
        if (const auto* n = std::get_if<form::field::Number>(&f.value))
            p.prose_width = static_cast<int>(n->value);
    }
}

// The theme the browser is currently highlighting. Empty == native, which is
// also what an out-of-range index yields — the safe end of the range, since
// native is the one choice correct on every terminal.
[[nodiscard]] std::string highlighted_theme(const pn::Appearance& o) {
    const auto names = pn::matching_themes(o.pane.picker.query);
    if (names.empty()) return {};
    const auto i = static_cast<std::size_t>(
        std::clamp(o.pane.picker.index, 0, static_cast<int>(names.size()) - 1));
    return names[i];
}

// Move the browser's highlight and LIVE-APPLY what it lands on. The applying
// is the point: a picker that only previews on commit makes you take the
// scheme to find out what it looks like.
void move_highlight(Model& m, int delta) {
    auto* o = m.ui.panel.get<pn::Appearance>();
    if (!o) return;
    const auto names = pn::matching_themes(o->pane.picker.query);
    const int n = static_cast<int>(names.size());
    if (n == 0) return;
    // Wraps, like every other list in the app: the way back to the top of 57
    // schemes should not be 56 keystrokes.
    o->pane.picker.index = ((o->pane.picker.index + delta) % n + n) % n;
    m.d.ui.theme = highlighted_theme(*o);
    reproject(m);
}

}  // namespace

Step appearance_update(Model m, msg::AppearanceMsg am) {
    return std::visit(overload{

        [&](OpenAppearance&) -> Step {
            pn::Appearance o;
            o.pane.form = pn::build_appearance_form(m.d.ui, /*tty=*/true);
            // Land on the first real setting, not the "Theme" section
            // header — a cursor parked on a row that does nothing reads as
            // a broken pane for the one keystroke it takes to notice.
            for (int i = 0; i < static_cast<int>(o.pane.form.fields.size()); ++i)
                if (!o.pane.form.fields[static_cast<std::size_t>(i)].is_header()) {
                    o.pane.form.cursor = i;
                    break;
                }
            m.ui.panel = std::move(o);
            m.ui.appearance_scroll.y = 0;
            return done(std::move(m));
        },

        [&](CloseAppearance&) -> Step {
            // Esc unwinds one level — to the palette snapshot stashed at
            // open, or to the thread. Nothing to save on the way out: it
            // was all saved as it was typed.
            ascend(m);
            return done(std::move(m));
        },

        [&](AppearanceKey& e) -> Step {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return done(std::move(m));

            // The browser owns the keyboard while it is up.
            if (o->pane.picking) return done(std::move(m));

            const auto applied = form::keys::apply(o->pane.form, e.action);

            if (applied.close)
                return agentty::app::update(std::move(m), Msg{CloseAppearance{}});

            // A Pick row asked for its picker. Only one row is a Pick, so
            // the hand-off needs no dispatch.
            if (applied.hand_off)
                return agentty::app::update(std::move(m), Msg{AppearancePickTheme{}});

            // Everything else: read the focused row back onto the prefs and
            // write through. `changed` covers ←/→ in place and a dropdown
            // commit; `left_field` covers finishing a number edit.
            if (applied.changed || applied.left_field) {
                if (const auto* row = o->pane.form.focused()) pull_field(m, *row);
                persist(m);
                reproject(m);
            }
            return done(std::move(m));
        },

        // ── The theme browser ────────────────────────────────────────

        [&](AppearancePickTheme&) -> Step {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return done(std::move(m));
            o->pane.picking      = true;
            o->pane.picker.query.clear();
            o->pane.picker.scroll = 0;
            // Open ON the theme in use, so the first thing the list shows is
            // where you already are — arrowing from there is a comparison
            // rather than a search.
            const auto names = pn::matching_themes({});
            const auto it = std::find(names.begin(), names.end(), m.d.ui.theme);
            o->pane.picker.index = it == names.end()
                ? 0 : static_cast<int>(std::distance(names.begin(), it));
            // What to restore if the user changes their mind. Stashed on the
            // pane rather than in a message so an Esc always has something
            // to go back to, however the browser was left.
            o->pane.picker.restore = m.d.ui.theme;
            return done(std::move(m));
        },

        [&](AppearanceThemeMove& e) -> Step {
            move_highlight(m, e.delta);
            return done(std::move(m));
        },

        [&](AppearanceThemeQuery& e) -> Step {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return done(std::move(m));
            if (e.text.empty()) {
                if (!o->pane.picker.query.empty()) o->pane.picker.query.pop_back();
            } else {
                o->pane.picker.query += e.text;
            }
            // Re-filtering invalidates the index; go back to the top rather
            // than to a position that means something else now.
            o->pane.picker.index  = 0;
            o->pane.picker.scroll = 0;
            // Live-apply the new top match. Typing narrows AND previews, so
            // "dra" shows you Dracula without a second keystroke.
            m.d.ui.theme = highlighted_theme(*o);
            reproject(m);
            return done(std::move(m));
        },

        [&](AppearanceThemeCommit&) -> Step {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return done(std::move(m));
            // The theme is already applied (the highlight applied it); Enter
            // only ends the browse and makes it durable.
            m.d.ui.theme = highlighted_theme(*o);
            o->pane.picking = false;
            persist(m);
            reproject(m);
            return done(std::move(m));
        },

        [&](AppearanceThemeCancel&) -> Step {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return done(std::move(m));
            // A true cancel: put back the theme we opened on, including on
            // disk — the live previews may have persisted nothing, but the
            // model must not keep the last one we merely looked at.
            m.d.ui.theme = o->pane.picker.restore;
            o->pane.picking = false;
            persist(m);
            reproject(m);
            return done(std::move(m));
        },

    }, am);
}

}  // namespace agentty::app::detail
