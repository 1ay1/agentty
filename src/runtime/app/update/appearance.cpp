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

#include "agentty/domain/ui_theme.hpp"
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

// Rebuild the sealed transcript so a theme change reaches it.
//
// m.ui.frozen holds BUILT Elements — turns sealed once and never rebuilt,
// because their height must stay byte-stable for the inline scrollback
// ledger. Colour is resolved when an Element is built, so those turns keep
// whatever theme was in force at seal time: pick a new scheme with a
// conversation on screen and the chrome restyles while the transcript above
// it does not. The taller the thread, the more of the screen stays wrong.
//
// rehydrate_frozen is the documented escape hatch for exactly this (see
// frozen.cpp's lifecycle invariants: "if such a mutation becomes necessary
// … call rehydrate_frozen() to rebuild from scratch"). It re-seals every
// turn under the theme now in force.
//
// Safe because a theme change is HEIGHT-PRESERVING: only the colours of
// each cell differ, so every rebuilt block lands at the height the ledger
// already recorded. A STRUCTURAL pref (compact turns, density) must not do
// this — it would re-seal at a new height and tear the ledger, which is why
// those settings are documented as forward-only.
void restyle_sealed_turns(Model& m) {
    // Publish FIRST. The reducer runs before view(), which is where the
    // theme is normally resolved and published — so rebuilding here without
    // this would re-seal every turn under the theme we are leaving, which
    // is the exact bug being fixed, one frame later. The turn builders read
    // `ui::` tokens, and those resolve through the live theme at BUILD time.
    //
    // publish_theme drives BOTH sinks (see ui_theme.hpp). That matters here
    // more than anywhere: rehydrate_frozen() below rebuilds sealed turns
    // immediately, and markdown prose renders from a PROJECTED palette
    // (~74 `colors::` reads in render_block.cpp alone) that is re-derived
    // by maya's on_theme_changed hook. Publishing only agentty's half left
    // that projection stale, so the rebuild baked the OUTGOING palette into
    // the new Elements — on a dark→light swap, dark ink on a light canvas
    // for the whole transcript, with only the newest unsealed message
    // looking right.
    //
    // Cheap and idempotent: view() publishes the same value again next
    // frame, and publish is a pointer store plus a value compare.
    ui_prefs::publish_theme(*ui_prefs::resolve(m.d.ui, /*tty=*/true).theme);

    // The LIVE tail is cached too, and for the same reason it is stale.
    //
    // ViewCache::finalized holds a built maya::Element per settled-but-not-
    // yet-frozen message — colours already resolved, exactly like a frozen
    // turn. It is keyed on message identity and nothing else, so a theme
    // change does not perturb the key and every subsequent frame serves the
    // old palette from cache. That is the last few turns of the
    // conversation: the part the user is actually looking at while they
    // cycle schemes.
    //
    // Cleared unconditionally, and BEFORE the early return below, because
    // this is the case that early return got wrong: a thread with nothing
    // frozen yet (a short conversation, or a fresh one) has no sealed rows
    // to rebuild but does have a live tail to re-colour, and it used to
    // return having done neither. Re-caching is one rebuild of the visible
    // tail, which is what picking a theme asked for.
    //
    // Settled entries only: a PINNED entry holds the live reveal widget for
    // the turn being streamed right now, and dropping that would restart
    // its animation mid-word. It rebuilds from its widget every frame, so
    // it picks up the new theme without being destroyed.
    m.ui.view_cache.clear_settled();

    if (m.ui.frozen_through == 0) return;
    rehydrate_frozen(m);
}

// Apply a row's current form value back onto the prefs.
//
// Keyed by row id through a total switch over the ids the builder emits: a
// positional table would have to be kept in the builder's order by hand, and
// an id that no row carries is a silently dead setting the compiler cannot
// see. (Hence the named kAp* constants on both sides.)
//
// Returns whether the row that changed affects RESOLVED COLOUR, so the
// caller knows to re-render what is already on screen. Theme is not the only
// such row: tier decides whether a scheme renders as truecolor, 256, 16 or
// not at all; polarity picks which side of the canvas the inks sit on; and
// syntax highlighting repaints every code block. All three reach the same
// already-built Elements the theme does, and all three used to change
// nothing until the next turn redrew.
//
// The structural rows (density, compact turns) are deliberately NOT in this
// set — see restyle_sealed_turns: they change row HEIGHTS, and re-sealing a
// frozen turn at a new height tears the scrollback ledger.
[[nodiscard]] bool pull_field(Model& m, const form::Field& f) {
    const auto idx = [&]() -> int {
        const auto* c = std::get_if<form::field::Choice>(&f.value);
        return c ? c->normalized(c->index) : 0;
    };
    const auto on = [&]() -> bool {
        const auto* t = std::get_if<form::field::Toggle>(&f.value);
        return t && t->on;
    };

    up::Prefs& p = m.d.ui;
    if (f.id == pn::kApTier)            { p.tier     = static_cast<up::ColorTier>(idx()); return true; }
    else if (f.id == pn::kApPolarity)   { p.polarity = static_cast<up::Polarity>(idx());  return true; }
    else if (f.id == pn::kApSyntax)     { p.syntax   = on();                              return true; }
    else if (f.id == pn::kApDensity)    p.density     = static_cast<up::Density>(idx());
    else if (f.id == pn::kApMotion)     p.motion      = static_cast<up::Motion>(idx());
    else if (f.id == pn::kApToolOutput) p.tool_output = static_cast<up::ToolOutput>(idx());
    else if (f.id == pn::kApThinking)   p.thinking    = static_cast<up::Thinking>(idx());
    else if (f.id == pn::kApTimestamps) p.timestamps  = static_cast<up::Timestamps>(idx());
    else if (f.id == pn::kApCompact)    p.compact_turns = on();
    else if (f.id == pn::kApProseWidth) {
        if (const auto* n = std::get_if<form::field::Number>(&f.value))
            p.prose_width = static_cast<int>(n->value);
    }
    return false;
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
    restyle_sealed_turns(m);
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
                bool recolour = false;
                if (const auto* row = o->pane.form.focused())
                    recolour = pull_field(m, *row);
                // A colour-bearing knob has to reach the transcript that is
                // already on screen, exactly as a theme change does —
                // otherwise dropping to 16 colours or flipping polarity
                // leaves every settled turn in the palette it was built
                // under, and only new turns look right.
                if (recolour) restyle_sealed_turns(m);
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
            restyle_sealed_turns(m);
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
            restyle_sealed_turns(m);
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
            restyle_sealed_turns(m);
            persist(m);
            reproject(m);
            return done(std::move(m));
        },

    }, am);
}

}  // namespace agentty::app::detail
