// appearance_update — reducer for the Appearance pane.
//
// How agentty looks: theme, colour tier, background polarity, density,
// motion, chrome. One pane, one rule — EVERY row is live.
//
// ── Why there is no apply step ───────────────────────────────────────────
// A theme is judged by LOOKING at it. If choosing and seeing are separated
// by a save keystroke (never mind a restart) the only way to evaluate a
// scheme is to commit to it first, which is exactly backwards. So each row
// writes through on the keystroke that changes it: mutate `m.d.ui()`, persist
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

// Persist the prefs to the USER store.
//
// Goes through persist_settings rather than saving `m.d.persisted` straight:
// only `ui` is aliased into the record (Domain::ui() returns persisted.ui).
// `effort`, `model_id`, `profile`, `smart` and the active provider live as
// SEPARATE Domain fields, so a raw save of the record wrote whatever those
// held at init() time and silently reverted every change made since — a
// theme tweak would undo a model switch. persist_settings syncs the scattered
// fields into the record first, then saves, then writes it back.
//
// Still write-behind at the Deps seam, so a reducer never stalls a frame on
// the disk; that is what makes saving on every keystroke affordable.
void persist(Model& m) {
    persist_settings(m);
}

// Rebuild the pane's rows from the prefs. The form is a PROJECTION of
// `m.d.ui()`, never a second copy of it: the reducer edits the prefs and
// re-derives, so there is no path on which the rows and the truth disagree.
// Cursor and focus are carried across because the user's place in the list
// is not part of what changed.
void reproject(Model& m) {
    auto* o = m.ui.panel.get<pn::Appearance>();
    if (!o) return;
    const int cursor = o->pane.form.cursor;
    auto focus = o->pane.form.focus;
    o->pane.form = pn::build_appearance_form(m.d.ui(), /*tty=*/true);
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
// THAT IS HISTORY. Colours are late-bound now (docs/LATE_BINDING.md): a
// sealed Element carries symbolic slots and resolves at PAINT, so it follows
// the live theme without being rebuilt. The paragraph above is kept because
// it names the failure precisely, and because the reasoning below — about
// what a re-seal costs — is exactly why we stopped doing one.
}  // namespace

// Publish a NEW theme to everything already on screen.
//
// Declared in internal.hpp: external linkage so its cost stays measurable
// (theme_preview_cost_probe), because this runs on EVERY arrow key in the
// theme browser — "moving applies it, you are looking at the preview" is that
// panel's whole design.
//
// ── Why this is now a publish and not a rebuild ───────────────────────
//
// It used to re-seal the frozen prefix through rehydrate_frozen() and drop
// every settled turn's built Element, because both had the outgoing palette
// RESOLVED into them and nothing else would ever invalidate them (a theme
// switch changes no bytes, so no content-keyed cache notices).
//
// Two things were wrong with paying that cost, and the second one is the
// reason the whole approach was abandoned:
//
//   1. It is unnecessary under late binding. The Elements are not stale.
//
//   2. It scales with the transcript, and a preview keystroke does not have
//      that much time. Re-rendering committed content per switch measured
//      2.42 -> 4.65 ms per keypress on a 100-message thread and grew with
//      length; past one key-repeat slot (33 ms at 30/s) keys outrun frames,
//      frames coalesce, and the browser visibly updates on every SECOND
//      theme you arrow past. "Slow" and "skips every other entry" are one
//      symptom, and this was its cause.
//
// Height stability is why a re-seal is dangerous rather than merely slow: a
// rebuild at a different height shifts maya's committed scrollback prefix,
// check_scrollback fails, the frame demotes to Stale, and recovery is a
// full-viewport repaint (~33 KB instead of 13 bytes). At 30 keys/sec that is
// ~1 MB/s the terminal cannot composite. Not rebuilding at all is both
// cheaper and safer than rebuilding carefully.
//
// A STRUCTURAL pref (density, compact turns) is different in kind: it
// changes row HEIGHTS, which invalidates the ledger's recorded measurements
// rather than their colours. Those stay forward-only — they apply to turns
// rendered from then on, and are deliberately not retro-applied, because
// re-sealing at a new height tears the ledger. See pull_field below.
void restyle_sealed_turns(Model& m) {
    // Publish the theme. That is now ALMOST the whole job.
    //
    // Colours are LATE-BOUND (docs/LATE_BINDING.md): a built Element carries
    // symbolic `Color::slot(...)` values, and maya resolves them in
    // Style::to_sgr() against whatever theme is live at PAINT time. So a
    // sealed turn built ten themes ago paints correctly under the current
    // one without being touched, and StylePool::retheme() re-derives the
    // cached SGR bytes on the swap — ids stay valid, no canvas cell needs
    // rewriting.
    //
    // This function used to rebuild the entire sealed transcript on every
    // arrow key, because a committed Element really did have the outgoing
    // palette baked into it. That rebuild was O(transcript) per keystroke,
    // and on a long thread it cost more than a key-repeat slot — keys
    // outran frames, frames coalesced, and the browser visibly updated on
    // every SECOND theme you passed. "Slow" and "skips entries" were one
    // symptom. Late binding deletes the work instead of optimising it.
    //
    // publish_theme drives BOTH sinks (see ui_theme.hpp): agentty's own
    // token atom and maya::app_set_theme, which bumps the theme epoch and
    // re-derives every projected palette. Publishing only one half is its
    // own bug — markdown prose renders from a projection, so a half-publish
    // left prose on the old scheme while the chrome moved.
    //
    // Cheap and idempotent: view() publishes the same value again next
    // frame, and publish is a pointer store plus a value compare.
    ui_prefs::publish_theme(*ui_prefs::resolve(m.d.ui(), /*tty=*/true).theme);

    // NOTHING ELSE TO DO.
    //
    // This is the part worth reading, because it used to be forty lines of
    // cache surgery and the surgery is what made theme switching slow.
    //
    // It used to drop ViewCache::finalized (the built Element per settled
    // message) and then call rehydrate_frozen() to rebuild the sealed
    // prefix, because both held Elements with the outgoing palette RESOLVED
    // into them. Neither is stale any more: those Elements carry symbolic
    // slots, so they paint under the theme live at paint time. Dropping and
    // rebuilding them would re-derive, at O(transcript) per keystroke,
    // something that is already correct.
    //
    // The one thing this path deliberately does NOT handle is a change to
    // row HEIGHTS (density, compact turns). Those are forward-only by
    // design: re-sealing a frozen turn at a new height tears the scrollback
    // ledger, so they take effect on turns rendered from then on rather
    // than retroactively. See pull_field's note below.
}

// Re-render what is already on screen, for a pref that changes the CONTENT
// of a built Element rather than only which colour its slots resolve to.
//
// A theme swap needs none of this (see restyle_sealed_turns): slots resolve
// at paint, so a stored Element follows the new palette untouched. These
// rows are different in kind:
//
//   • tier      — decides whether a scheme renders as truecolor, 256, 16 or
//                 not at all. That is a QUANTISATION of the resolved value,
//                 applied when the Element is built, so an already-built one
//                 keeps the old approximation.
//   • polarity  — picks which side of the canvas the inks sit on, which
//                 changes which theme resolve() hands back entirely.
//   • syntax    — re-tokenises every code block into a different span tree.
//                 Not a colour change at all; a structural one.
//
// All three reach the same already-built Elements, and all three used to
// change nothing until the next turn redrew.
void rebuild_rendered_content(Model& m) {
    ui_prefs::publish_theme(*ui_prefs::resolve(m.d.ui(), /*tty=*/true).theme);
    m.ui.view_cache.clear_settled();
    if (m.ui.frozen_through == 0) return;
    rehydrate_frozen(m);
}

namespace {

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

    up::Prefs& p = m.d.ui();
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
// also what an empty list yields — the safe end of the range, since native is
// the one choice correct on every terminal.
//
// Reads through the picker's own accessor: the cursor is clamped inside
// against the filtered set, so there is no second place that can disagree
// about which row is selected.
[[nodiscard]] std::string highlighted_theme(const pn::Appearance& o) {
    const std::string* sel = o.pane.picker.picker.selected();
    return sel ? *sel : std::string{};
}

// Move the browser's highlight and LIVE-APPLY what it lands on. The applying
// is the point: a picker that only previews on commit makes you take the
// scheme to find out what it looks like.
//
// Only the SEALED transcript is expensive to restyle — restyle_sealed_turns()
// drops the settled view cache and re-parses each kept message's markdown,
// measured at 37-82 ms on a real thread because the following render is then
// necessarily cold. Everything else (the browser itself, the composer, the
// status bar) repaints from the published theme for free.
//
// So the rebuild is skipped when there is nothing sealed to rebuild, which
// is the common case people actually browse in: a fresh session, or one
// short enough that nothing has frozen yet. The theme is published either
// way, so the preview is identical — only the catch-up for already-built
// rows is conditional.
void move_highlight(Model& m, int delta) {
    auto* o = m.ui.panel.get<pn::Appearance>();
    if (!o) return;
    // The picker wraps and clamps against its OWN filtered count, so the
    // reducer never derives list geometry. That is what keeps the cursor and
    // the rendered window from drifting apart — there is only one of them.
    // Wrapping, because walking back to the top of 615 schemes should not
    // take 614 keystrokes.
    o->pane.picker.picker.move_wrapping(delta);

    std::string next = highlighted_theme(*o);
    // Landing on the scheme already in force is a no-op, not a rebuild. With
    // 615 rows a wrap or a repeated key hits this often enough to matter.
    if (next == m.d.ui().theme) { reproject(m); return; }

    m.d.ui().theme = std::move(next);
    // Re-style everything on screen, including the frozen prefix — it is
    // painted every frame, so a preview that skipped it would show the new
    // scheme on the newest turns and the old one above. Bounded to ~3
    // viewports by frozen_row_budget(), so this is O(visible), not
    // O(transcript): ~0.64 ms on a 3000-message thread.
    restyle_sealed_turns(m);
    reproject(m);
}

}  // namespace

Cmd appearance_update(Model& m, msg::AppearanceMsg am) {
    return std::visit(overload{

        [&](OpenAppearance&) -> Cmd {
            pn::Appearance o;
            o.pane.form = pn::build_appearance_form(m.d.ui(), /*tty=*/true);
            // Land on the first real setting, not the "Theme" section
            // header — a cursor parked on a row that does nothing reads as
            // a broken pane for the one keystroke it takes to notice.
            for (int i = 0; i < static_cast<int>(o.pane.form.fields.size()); ++i)
                if (!o.pane.form.fields[static_cast<std::size_t>(i)].is_header()) {
                    o.pane.form.cursor = i;
                    break;
                }
            m.ui.panel.descend(std::move(o));
            m.ui.appearance_scroll.y = 0;
            return Cmd::none();
        },

        [&](CloseAppearance&) -> Cmd {
            // Esc unwinds one level — to the palette snapshot stashed at
            // open, or to the thread. Nothing to save on the way out: it
            // was all saved as it was typed.
            ascend(m);
            return Cmd::none();
        },

        [&](AppearanceKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return Cmd::none();

            // The browser owns the keyboard while it is up.
            if (o->pane.picking) return Cmd::none();

            const auto applied = form::keys::apply(o->pane.form, e.action);

            if (applied.close)
                return appearance_update(m, msg::AppearanceMsg{CloseAppearance{}});

            // A Pick row asked for its picker. Only one row is a Pick, so
            // the hand-off needs no dispatch.
            if (applied.hand_off)
                return appearance_update(m, msg::AppearanceMsg{AppearancePickTheme{}});

            // Everything else: read the focused row back onto the prefs and
            // write through. `changed` covers ←/→ in place and a dropdown
            // commit; `left_field` covers finishing a number edit.
            if (applied.changed || applied.left_field) {
                bool rebuild = false;
                if (const auto* row = o->pane.form.focused())
                    rebuild = pull_field(m, *row);
                // tier / polarity / syntax change the CONTENT of an
                // already-built Element, not just which colour its slots
                // resolve to — a quantisation, a different resolved theme,
                // or a different span tree. A theme swap needs no rebuild
                // (slots resolve at paint); these do.
                if (rebuild) rebuild_rendered_content(m);
                persist(m);
                reproject(m);
            }
            return Cmd::none();
        },

        // ── The theme browser ────────────────────────────────────────

        [&](AppearancePickTheme&) -> Cmd {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return Cmd::none();
            o->pane.picking      = true;
            o->pane.picker.picker.clear_query();
            // Open ON the theme in use, so the first thing the list shows is
            // where you already are — arrowing from there is a comparison
            // rather than a search. jump_to clamps inside the picker against
            // its own filtered count, and the viewport is derived from that
            // same cursor, so the row we open on is one the window contains.
            {
                const auto& pk  = o->pane.picker.picker;
                const auto& src = pk.entries();
                const auto& idx = pk.filtered();
                for (std::size_t i = 0; i < idx.size(); ++i) {
                    if (src[idx[i]] == m.d.ui().theme) {
                        pk.jump_to(static_cast<int>(i));
                        break;
                    }
                }
            }
            // What to restore if the user changes their mind. Stashed on the
            // pane rather than in a message so an Esc always has something
            // to go back to, however the browser was left.
            o->pane.picker.restore = m.d.ui().theme;
            return Cmd::none();
        },

        [&](AppearanceThemeMove& e) -> Cmd {
            move_highlight(m, e.delta);
            return Cmd::none();
        },

        [&](AppearanceThemeQuery& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return Cmd::none();
            if (e.text.empty()) o->pane.picker.picker.backspace();
            else                o->pane.picker.picker.type(e.text);
            // The picker resets its own cursor on every query edit — one
            // place, so no arm can narrow the list and leave the cursor
            // pointing at a row that now means something else.
            //
            // Live-apply the new top match. Typing narrows AND previews, so
            // "dra" shows you Dracula without a second keystroke.
            m.d.ui().theme = highlighted_theme(*o);
            restyle_sealed_turns(m);
            reproject(m);
            return Cmd::none();
        },

        [&](AppearanceThemeCommit&) -> Cmd {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return Cmd::none();
            // The theme is already applied (the highlight applied it); Enter
            // only ends the browse and makes it durable.
            m.d.ui().theme = highlighted_theme(*o);
            o->pane.picking = false;
            // The full restyle, once, on the way out. Browsing already
            // re-styled on each move (the list is its own preview), so this
            // is the settle: it puts the sealed transcript under whichever
            // scheme you actually stopped on.
            restyle_sealed_turns(m);
            persist(m);
            reproject(m);
            return Cmd::none();
        },

        [&](AppearanceThemeCancel&) -> Cmd {
            auto* o = m.ui.panel.get<pn::Appearance>();
            if (!o) return Cmd::none();
            // A true cancel: put back the theme we opened on, including on
            // disk — the live previews may have persisted nothing, but the
            // model must not keep the last one we merely looked at.
            m.d.ui().theme = o->pane.picker.restore;
            o->pane.picking = false;
            // Full restyle on the way out, as with Commit: the frozen prefix
            // may be carrying a previewed scheme's colours and has to be put
            // back under the restored one.
            restyle_sealed_turns(m);
            persist(m);
            reproject(m);
            return Cmd::none();
        },

    }, am);
}

}  // namespace agentty::app::detail
