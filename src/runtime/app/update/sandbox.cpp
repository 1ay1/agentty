// sandbox_update — reducer for the Sandbox pane.
//
// What a command may touch: filesystem scope, network, syscall profile,
// resource caps, and the trust-handoff policy.
//
// ── Why this pane is NOT live, when Appearance is ────────────────────────
//
// Appearance writes through on every keystroke, because a theme is judged by
// looking at it and there must be nothing between choosing and seeing.
//
// A boundary is the opposite. Three reasons it has an explicit Save:
//
//   1. A sandbox is judged by READING what it promises, not by watching it.
//      The pane compiles each edit into a wall report -- "filesystem.write:
//      strong via landlock abi 10" -- so the thing you evaluate is the
//      report, and the report is available before anything is committed.
//
//   2. A half-applied policy is one nobody can reason about. Typing a port
//      list one digit at a time would, live, pass through "allow port 4",
//      "allow port 44", "allow port 443" -- and the first two are real
//      policies that were never asked for.
//
//   3. It must not change under a command that is already running. The
//      config is installed set-once at spawn time (see sandbox.cpp's g_cfg),
//      so a mid-flight edit would give two commands in one session two
//      different boundaries with nothing saying which got which.
//
// So edits accumulate in the form, the preview tracks them, and Save commits
// the whole config at once.
//
// ── Why the preview recompiles on every edit anyway ──────────────────────
//
// The pane's entire reason to exist is that a boundary you cannot observe is
// one you cannot trust -- agentty has shipped "sandbox: active" while every
// command died on a uid_map denial (issue #21). claybin's compile() is pure
// over a described host, so the walls can be recomputed without spawning
// anything: it is a compile, not a probe. That makes it cheap enough to run
// on each keystroke, and running it there is what keeps the report from ever
// describing a config the rows no longer say.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/tool/util/sandbox.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/sandbox.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/tool/util/sandbox_claybin.hpp"
#if defined(__linux__)
#include <claybin/plan/compile.hpp>   // probe_host() -> HostCapabilities
#endif

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

namespace {

namespace sb = agentty::tools::util::sandbox;

// What the HOST can do, which decides which rows are even offered. A pane
// that showed per-port network on a kernel without landlock abi 4 would be
// offering a control that silently does nothing -- so the rows that need a
// capability are marked unavailable rather than hidden. Hiding them would
// make the limitation invisible, which is the failure mode this whole pane
// exists to avoid.
[[nodiscard]] bool claybin_here() {
#if defined(__linux__) || defined(__APPLE__)
    // The real probe on both platforms, and real in the same sense: on Linux
    // it forks and attempts the uid_map write; on macOS it forks and actually
    // calls sandbox_init(). Neither guesses from a feature flag, because
    // guessing is how issue #21 happened -- "sandbox: active" while every
    // command died on the denial.
    return sb::claybin_backend::available();
#else
    // claybin compiles a plan on every platform but can only APPLY one on
    // Linux and macOS, so elsewhere the answer is no regardless of the
    // library being linked.
    return false;
#endif
}

[[nodiscard]] std::uint32_t landlock_abi_here() {
#if defined(__linux__)
    return ::clay::probe_host().landlock_abi;   // 0 = absent
#else
    return 0;
#endif
}

// Everything the pane needs to know about reality, in one place.
//
// The `running` field is the one that was missing and caused the subtitle to
// lie: it comes from `sb::detected_backend()`, the engine actually confining
// commands in THIS process, which the seal makes immutable. It is not derived
// from the config and must never be -- the config is what the user wants, this
// is what they have.
[[nodiscard]] pn::HostFacts host_facts_here() {
    pn::HostFacts f;
    f.claybin_available = claybin_here();
    f.landlock_abi = landlock_abi_here();
    f.sandbox_active = sb::is_active();
    f.mode_off = sb::requested_mode() == sb::Mode::Off;
    f.running = sb::detected_backend() == sb::Backend::Claybin
                    ? sandbox_cfg::LinuxBackend::Claybin
                    : sandbox_cfg::LinuxBackend::Bwrap;
    return f;
}

// Recompute the walls from the form. Runs after every edit, so the report
// and the rows cannot disagree.
//
// Annotation happens HERE rather than in build_sandbox_form because it needs
// the compile result, and the compile needs the finished config. Doing it in
// the same function as the compile is what keeps the per-row wall from going
// stale: there is no path that reprices without re-annotating.
void reprice(pn::Sandbox& o) {
    const auto cfg = pn::read_sandbox_form(o.pane.form, o.pane.working);
    // The rows are a projection of THIS config, so it is what the next
    // readback must build on -- otherwise the settings that live outside the
    // form (the path and port lists) would be re-read from a stale base and
    // every list edit would evaporate on the next keystroke.
    o.pane.working = cfg;
    o.pane.preview = pn::preview_sandbox(cfg);
    // The status decides the TENSE of every annotation: walls for an engine
    // that is not running are a forecast and have to read as one.
    pn::annotate_sandbox_form(o.pane.form, o.pane.preview, cfg,
                              pn::engine_status(cfg, o.pane.facts));
    // And the answer to "will this actually work when I restart". Computed
    // HERE, with the preview that was just built from the same cfg, so the
    // verdict and the walls it is judging cannot disagree.
    o.pane.restart_note = pn::restart_outcome(cfg, o.pane.facts, o.pane.preview);
}

// Rebuild the rows from a config, keeping the user's place in the list --
// where the cursor sits is not part of what changed.
void reproject(pn::Sandbox& o, const sandbox_cfg::Config& cfg) {
    const int cursor = o.pane.form.cursor;
    auto focus = o.pane.form.focus;
    o.pane.working = cfg;
    // Re-measure and STORE. The view needs these to answer "will this work on
    // restart" and must not probe for itself -- a uid_map fork mid-render is
    // not something a pure view may do.
    o.pane.facts = host_facts_here();
    o.pane.form = pn::build_sandbox_form(cfg, o.pane.facts);
    o.pane.form.cursor = std::clamp(cursor, 0,
        std::max(0, static_cast<int>(o.pane.form.fields.size()) - 1));
    o.pane.form.focus = focus;
    reprice(o);
}

}  // namespace

Cmd sandbox_update(Model& m, msg::SandboxMsg sm) {
    return std::visit(overload{

        [&](OpenSandbox&) -> Cmd {
            pn::Sandbox o;
            const auto facts = host_facts_here();
            o.pane.facts = facts;
            o.pane.claybin_available = facts.claybin_available;
            // Kept for anything that wants the running engine as a string.
            // Note the FORM no longer derives its subtitle from the selection
            // -- it asks `facts` -- which is what stopped the header claiming
            // seccomp on a bwrap process.
            o.pane.backend = !facts.sandbox_active
                ? "none"
                : (facts.running == sandbox_cfg::LinuxBackend::Claybin ? "claybin"
                                                                       : "bwrap");

            // Seeded from the SAVED policy (m.d.persisted), not the live one.
            //
            // These differ, and only since the policy became sealed: the
            // live config is frozen at whatever this process started with,
            // while `persisted` holds any save made during this session. If
            // the pane opened on the live one it would silently discard an
            // earlier save -- you would set a syscall profile, reopen, and
            // find your edit gone.
            //
            // `configured` is the discriminator: an untouched config means
            // nobody has been here, so fall back to what is actually
            // enforcing rather than to struct defaults.
            const auto& seed = m.d.persisted.sandbox.configured
                                   ? m.d.persisted.sandbox
                                   : sb::config();
            o.pane.form = pn::build_sandbox_form(seed, facts);
            o.pane.working = seed;

            // Already-saved-this-session is worth carrying into the reopened
            // pane, so the "applies on restart" footer does not vanish just
            // because the overlay closed and came back.
            o.pane.saved_pending_restart =
                m.d.persisted.sandbox.configured &&
                !(m.d.persisted.sandbox == sb::config());

            // Land on the first real setting, not a section header -- a
            // cursor parked on a row that does nothing reads as broken for
            // the one keystroke it takes to notice.
            for (int i = 0; i < static_cast<int>(o.pane.form.fields.size()); ++i)
                if (!o.pane.form.fields[static_cast<std::size_t>(i)].is_header()) {
                    o.pane.form.cursor = i;
                    break;
                }
            reprice(o);
            m.ui.panel.descend(std::move(o));
            m.ui.sandbox_scroll.y = 0;
            return Cmd::none();
        },

        [&](CloseSandbox&) -> Cmd {
            // Esc discards. Deliberate, and the opposite of Appearance:
            // nothing was applied on the way in, so there is nothing to undo
            // -- and a boundary that committed on Esc would be a boundary
            // changed by walking away from the screen.
            ascend(m);
            return Cmd::none();
        },

        [&](SandboxKey& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Sandbox>();
            if (!o) return Cmd::none();

            // Which row the key landed on, captured BEFORE apply() -- an
            // arrow key can move the cursor, and we need the row that was
            // actually edited.
            const std::string row_id =
                (o->pane.form.cursor >= 0 &&
                 o->pane.form.cursor < static_cast<int>(o->pane.form.fields.size()))
                    ? o->pane.form.fields[static_cast<std::size_t>(o->pane.form.cursor)].id
                    : std::string{};

            const auto applied = form::keys::apply(o->pane.form, e.action);

            if (applied.close)
                return sandbox_update(m, msg::SandboxMsg{CloseSandbox{}});

            // Changing the ENGINE changes which rows can mean anything, so
            // it reprojects rather than just repricing: the locks and their
            // reasons are computed at build time. Without this you could
            // switch to claybin and the syscall row would still say "bwrap
            // cannot express this" until the pane was reopened.
            if (applied.changed && row_id == pn::kSbBackend) {
                reproject(*o, pn::read_sandbox_form(o->pane.form, o->pane.working));
                // ...and PERSIST, same as every other row. See the posture
                // branch below for why returning early here was a bug.
                return sandbox_update(m, msg::SandboxMsg{SandboxSave{}});
            }

            // The POSTURE row is the only one that writes OTHER rows, and it is
            // applied HERE rather than in readback because it is an ACTION, not
            // a value.
            //
            // Readback sees only values, and this row's value is the posture the
            // config used to be -- so it cannot distinguish "the user just
            // picked Hardened" from "the user picked Hardened earlier and has
            // since edited Memory". Inferring it there re-stamped the preset
            // over the user's edit on the next repaint, which is an edit
            // vanishing with no message. The reducer has the row id, so here the
            // distinction is free and exact.
            //
            // Reprojects rather than reprices for a stronger reason than the
            // engine row: the engine changes which rows are LOCKED, this changes
            // their VALUES. Repricing alone would leave 26 rows showing the old
            // policy while the footer described the new one, which is exactly
            // the rows/report disagreement this pane exists to prevent.
            if (applied.changed && row_id == pn::kSbPosture) {
                const auto chosen = static_cast<sandbox_cfg::Posture>(
                    std::get<form::field::Choice>(
                        o->pane.form.fields[static_cast<std::size_t>(
                            o->pane.form.cursor)].value).index);
                // Custom is a label the row REPORTS, not a command;
                // apply_posture returns the config untouched for it, so landing
                // on Custom while cycling leaves the rows alone.
                const auto cfg = sandbox_cfg::apply_posture(
                    pn::read_sandbox_form(o->pane.form, o->pane.working), chosen);
                reproject(*o, cfg);
                // AND SAVE. This returned Cmd::none(), which meant the one row
                // that rewrites every OTHER row was the one row that never
                // reached the autosave below -- so picking Hardened repainted
                // 26 rows, said "applies on restart", and persisted nothing.
                // Next launch loaded the old config and the posture row,
                // derived by comparison, correctly reported Custom.
                //
                // Reported as "I changed the preset to Hardened, restarted,
                // it was Custom again", and the derivation was right the whole
                // time: the config really was unchanged on disk.
                //
                // Not a special case worth its own save path -- it is the
                // same SandboxSave the generic branch sends. The early return
                // was the whole defect.
                return sandbox_update(m, msg::SandboxMsg{SandboxSave{}});
            }

            // A Pick row asked for its editor. The path and port rows are the
            // only Picks here, and each opens the same list editor -- which is
            // why the message carries the row id rather than there being four
            // near-identical messages.
            if (applied.hand_off && !row_id.empty())
                return sandbox_update(m, msg::SandboxMsg{SandboxEditList{row_id}});

            // No `fired` arm, and that is a property of the form rather than
            // an omission: this pane has no Action rows (the one thing to DO
            // is save, and that is a chord, not a row). If one is ever added,
            // apply() will report it here and it needs handling -- see
            // plugin_edit.cpp for fired.
            //
            // Re-price on ANY mutation. `changed` covers a dropdown move and
            // a toggle; `left_field` covers leaving a text/number row, which
            // is where those rows commit (obligation #4 in PANELS.md: a
            // half-typed value never rewrites config). Narrower than this and
            // the wall report describes a config the rows no longer say --
            // which is the whole thing this pane exists to prevent.
            if (applied.changed || applied.left_field) {
                reprice(*o);
                // AUTOSAVE.
                //
                // There is no ^S any more, and dropping it does not weaken the
                // seal. Two different things were being conflated:
                //
                //   persisting   writing the policy to disk
                //   applying     installing it as the live boundary
                //
                // Only the SECOND is dangerous mid-session (a boundary that
                // moves under a running command), and it still never happens --
                // set_config() stays sealed and this path never calls it. The
                // first is just a file write, and making the user remember a
                // chord for it only ever produced lost edits: every other pane
                // in agentty persists as you go.
                //
                // The footer already says the policy applies on restart, so
                // what the user sees is unchanged except that it is now true
                // without a keystroke.
                return sandbox_update(m, msg::SandboxMsg{SandboxSave{}});
            }
            return Cmd::none();
        },

        [&](SandboxRefreshPreview&) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::Sandbox>()) reprice(*o);
            return Cmd::none();
        },

        [&](SandboxSave&) -> Cmd {
            auto* o = m.ui.panel.get<pn::Sandbox>();
            if (!o) return Cmd::none();

            auto cfg = pn::read_sandbox_form(o->pane.form, o->pane.working);
            // `configured` is what keeps an upgrade from changing anyone's
            // boundary: persistence only applies a saved block when the user
            // has actually been here. Saving is the act that sets it.
            cfg.configured = true;

            // PERSIST ONLY. No set_config() here, deliberately.
            //
            // The live policy was sealed at startup and set_config() would
            // refuse this anyway (it logs and returns), so calling it would
            // be theatre -- but the reason it refuses is the point: a
            // boundary must not move under a process that is already running.
            // Switching claybin -> bwrap mid-session silently drops the
            // seccomp filter, the cgroup caps and the secret masks, and
            // re-tightening later does not un-read a key that already left.
            //
            // So this writes the policy for the NEXT launch and says so. The
            // pane's footer carries "applies on restart" (see the view), so
            // the user is never left believing a wall went up when it did
            // not. Restart is the apply step; that is the cost of a boundary
            // you can trust for the whole session.
            m.d.persisted.sandbox = cfg;

            // Re-derive the rows from the config we committed, so they show
            // the normalised result (ports sorted, a blank number as 0)
            // rather than the raw text that produced it. Seeded from `cfg`,
            // NOT from sb::config() -- the live policy is the old one and
            // reprojecting off it would throw the edit away.
            reproject(*o, cfg);
            o->pane.saved_pending_restart = true;
            return persist_settings(m);
        },

        [&](SandboxEditList& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Sandbox>();
            if (!o) return Cmd::none();

            // Seed from the CONFIG the form currently describes, not from the
            // saved policy: the user may have changed the posture (which
            // rewrites paths) without saving yet, and editing a stale list
            // would silently discard that.
            const auto cfg = pn::read_sandbox_form(o->pane.form, o->pane.working);

            const bool ports = e.row_id == pn::kSbPorts;
            const std::vector<std::string>* src = nullptr;
            std::vector<std::string> port_strings;
            std::string title, help;

            if (ports) {
                for (auto p : cfg.allow_ports) port_strings.push_back(std::to_string(p));
                src = &port_strings;
                title = "Allowed ports";
                help  = "only these ports may be reached \xc2\xb7 53 is DNS, and "
                        "forgetting it breaks name lookup for everything";
            } else if (e.row_id == pn::kSbReadPaths) {
                src = &cfg.read_paths;
                title = "Also readable";
                help  = "extra paths a command may READ, beyond the scope "
                        "setting \xc2\xb7 one per line";
            } else if (e.row_id == pn::kSbWritePaths) {
                src = &cfg.write_paths;
                title = "Also writable";
                help  = "extra paths a command may WRITE \xc2\xb7 the workspace is "
                        "already writable";
            } else if (e.row_id == pn::kSbDenyPaths) {
                src = &cfg.deny_paths;
                title = "Masked";
                help  = "made unreadable even inside the scope above \xc2\xb7 "
                        "credential files are masked whether listed or not";
            } else {
                return Cmd::none();   // not a list row
            }

            pn::SandboxList ed;
            ed.pane = pn::build_sandbox_list(e.row_id, std::move(title),
                                             std::move(help), *src, ports);
            // Remember the pane we came from so Esc returns to it rather than
            // closing the overlay stack -- the same WithFrom stash every other
            // nested pane uses.
            // descend(), not a bare assignment: it stashes the sandbox pane as
            // this editor's parent, so Esc returns to the pane rather than
            // exiting the overlay stack.
            m.ui.panel.descend(std::move(ed));
            return Cmd::none();
        },

        [&](SandboxListKey& e) -> Cmd {
            auto* ed = m.ui.panel.get<pn::SandboxList>();
            if (!ed) return Cmd::none();

            // THE invariant, re-established before anything reads it:
            // suggestions exist only while a field is live.
            //
            // apply() can end an edit session on the PREVIOUS keystroke
            // (MoveFirst, MoveLast, PageUp/PageDown and Activate all commit
            // the field and leave) without any of this arm's early returns
            // running. The flag then outlives its cause, and the next key is
            // interpreted against a list nobody can see -- Up/Down steering an
            // invisible selection instead of moving rows, Tab overwriting a
            // row nobody was editing.
            //
            // Asserting it HERE rather than at each exit is the difference
            // between a rule and a habit: every handler below gets a
            // consistent state, and a new one cannot forget to restore it. A
            // state sweep found this; reading the control flow did not.
            if (!ed->pane.form.editing()) ed->pane.completing = false;

            // Tab accepts the highlighted suggestion.
            //
            // Tab rather than Enter, because Enter already means "commit this
            // field and move on" everywhere in the form layer and stealing it
            // here would make the one list that has completion behave unlike
            // every other field. Tab has no other meaning in a form, so it
            // costs nothing.
            //
            // editing() for the same reason as the arrows below: this returns
            // early, so a stale flag would let Tab overwrite a row nobody was
            // editing.
            if (ed->pane.completing && ed->pane.form.editing() &&
                e.action.intent == form::keys::Intent::Complete) {
                if (const auto* hit = ed->pane.complete.selected()) {
                    if (auto* row = ed->pane.form.focused())
                        if (auto* t = std::get_if<form::field::Text>(&row->value)) {
                            t->value = *hit;
                            t->cursor = t->value.size();
                            ed->pane.form.edit_dirty = true;
                            // Sync the picker's query to what we just wrote.
                            //
                            // Otherwise the NEXT keystroke sees row text that
                            // differs from the query, decides the user typed
                            // something, and reopens the list -- now matching
                            // the full accepted path, so Tab had the effect of
                            // accepting a suggestion and immediately offering
                            // it back. Accepting must settle, not loop.
                            ed->pane.complete.clear_query();
                            ed->pane.complete.type(std::string_view{t->value});
                        }
                }
                ed->pane.completing = false;
                return Cmd::none();
            }

            // Esc, in one place, with a fixed order of what it cancels.
            //
            // Three things can be open at once (suggestions, a live field, the
            // editor) and Esc has to peel exactly one per press, outermost
            // last. Scattering that across apply()'s result and a follow-up
            // check is what made it unreliable: a press could dismiss the
            // suggestions AND be swallowed by the field, so the editor took
            // three Escs from some states and two from others.
            if (e.action.intent == form::keys::Intent::Close) {
                if (ed->pane.form.editing() || ed->pane.form.choosing()) {
                    // 1. the live field, AND any suggestions it opened.
                    //
                    // One press for both, because they are one layer from the
                    // user's side: the completion list only exists while a
                    // field is live, so charging a separate Esc for it just
                    // makes the editor take three presses to leave and the
                    // count vary with whether you had typed anything.
                    (void)form::keys::apply(ed->pane.form, e.action);
                    ed->pane.completing = false;
                    return Cmd::none();
                }
                if (ed->pane.completing) {
                    // 2. suggestions with no live field. Only reachable if a
                    //    future path opens them while browsing; dismiss and
                    //    stay, rather than leaving a stale list on screen.
                    ed->pane.completing = false;
                    return Cmd::none();
                }
                // 3. the editor itself.
                return sandbox_update(m, msg::SandboxMsg{SandboxListClose{}});
            }

            const auto applied = form::keys::apply(ed->pane.form, e.action);
            if (applied.close)
                return sandbox_update(m, msg::SandboxMsg{SandboxListClose{}});

            // ^X removes the focused entry outright.
            //
            // Clearing a line to delete it works (blanks are dropped on
            // commit) but it is not DISCOVERABLE -- "no way to remove the
            // entry" was the report, from someone looking at a list with no
            // delete affordance. The form layer already routes ^X as
            // ResetField and leaves the meaning to the pane, so this costs no
            // new key and no new modality.
            //
            // Removes the ROW, not just its text, because leaving an empty row
            // behind would make the list grow a gap every time you deleted
            // something. The trailing add row is never removed: it is the
            // affordance, not an entry.
            if (e.action.intent == form::keys::Intent::ResetField) {
                const int i = ed->pane.form.cursor;
                const int last = static_cast<int>(ed->pane.form.fields.size()) - 1;
                if (i >= 0 && i < last) {
                    ed->pane.form.fields.erase(
                        ed->pane.form.fields.begin() + i);
                    ed->pane.form.cursor = std::clamp(
                        i, 0,
                        std::max(0, static_cast<int>(ed->pane.form.fields.size()) - 1));
                    ed->pane.form.focus = form::focus::Browsing{};
                    ed->pane.completing = false;
                    pn::renumber_sandbox_list(ed->pane);
                }
                return Cmd::none();
            }

            // Arrows move the SUGGESTIONS while they are open.
            //
            // Guarded on editing() as well as `completing`, because this sits
            // BEFORE apply() and returns early: a stale flag from a session
            // that ended on the previous keystroke would make Up/Down steer an
            // invisible list instead of moving between rows, and the pane
            // would look frozen. The invariant is re-asserted after apply()
            // too, but an early return never reaches that.
            if (ed->pane.completing && ed->pane.form.editing() &&
                (e.action.intent == form::keys::Intent::MovePrev ||
                 e.action.intent == form::keys::Intent::MoveNext)) {
                ed->pane.complete.move_wrapping(
                    e.action.intent == form::keys::Intent::MoveNext ? +1 : -1);
                return Cmd::none();
            }

            // Anything that ENDED the edit session closes the suggestions
            // with it.
            //
            // Several intents end an edit inside apply() without being Esc --
            // MoveFirst, MoveLast, PageUp/PageDown and Activate all commit the
            // field and leave. The resync at the bottom of this arm recomputes
            // `completing` and would catch that, but only on paths that REACH
            // it; every early return above is a way for the flag to outlive
            // the session that justified it.
            //
            // Asserting it here, right after apply(), makes the invariant
            // "suggestions imply an edit session" hold at every exit rather
            // than at most of them. A state sweep found this; reading the
            // control flow did not.
            if (!ed->pane.form.editing()) ed->pane.completing = false;

            // Backspace on a browsing row ENTERS the edit session.
            //
            // Reported as "backspace on the selected row doesn't work first":
            // the editor opens in Browsing, so the first press hit apply()'s
            // editing guard and vanished. Typing a character auto-enters (the
            // form layer's type-to-edit) and Backspace did not, so the two
            // ways of starting to edit the same row disagreed.
            //
            // The form layer's reason for NOT auto-entering on delete keys is
            // sound where it applies -- a stray Backspace must never destroy
            // bytes the user cannot see a caret in -- but it does not apply
            // here: every row in this pane IS an editable value, there is
            // nothing else Backspace could mean, and the caret is on the
            // focused row already. So it edits, and a row with nothing in it
            // is simply left alone.
            if (e.action.intent == form::keys::Intent::Backspace &&
                !ed->pane.form.editing()) {
                if (auto* row = ed->pane.form.focused())
                    if (auto* t = std::get_if<form::field::Text>(&row->value);
                        t && !t->value.empty()) {
                        ed->pane.form.focus = form::focus::Editing{};
                        ed->pane.form.edit_dirty = false;
                        // Caret to the END first: a browsing row has whatever
                        // cursor it was built with, and deleting from position
                        // 0 would silently do nothing on a non-empty value.
                        t->cursor = t->value.size();
                        // Then DO the delete. apply() already ran above and
                        // dropped this key on its editing guard, so entering
                        // the session without acting would make the first
                        // press merely arm the second -- the same "doesn't
                        // work first" with one less step.
                        (void)form::keys::apply(ed->pane.form, e.action);
                    }
            }

            // Keep exactly ONE trailing blank row, and only ever ADD it.
            //
            // This used to rebuild the whole form whenever the entry count
            // disagreed with the row count, which made three separate bugs:
            //
            //   * backspacing a row to empty made it "absent", so the rebuild
            //     DELETED the row you were editing -- focus jumped, and the
            //     next backspace had no field to act on. That is the
            //     "backspace doesn't work sometimes".
            //   * clearing a row to remove it therefore could not work: the
            //     row came back (rebuilt from values that no longer had it) or
            //     vanished mid-keystroke.
            //   * every rebuild made fresh fields, so caret/edit state had to
            //     be copied back by hand and anything missed read as the
            //     editor resetting itself.
            //
            // Growing is the only safe edit while a field is live: appending a
            // row cannot disturb the one you are in. REMOVAL happens on
            // commit, where read_sandbox_list() drops blanks -- so clearing a
            // line still removes the entry, it just does not reshuffle the
            // form while you are typing in it.
            const bool last_row_used = [&] {
                if (ed->pane.form.fields.empty()) return true;
                // Every entry row is Text now, ports included -- a Number
                // widget cannot render a caret, so a list you type into must
                // not use one.
                const auto& back = ed->pane.form.fields.back().value;
                if (const auto* t = std::get_if<form::field::Text>(&back))
                    return !t->value.empty();
                return false;
            }();
            if (last_row_used) {
                const auto id = "e" + std::to_string(ed->pane.form.fields.size());
                ed->pane.form.fields.push_back(
                    ed->pane.numeric
                        ? pn::list_entry_number(id, "+", "type a port")
                        : pn::list_entry_text(id, "+", "type a path"));
                pn::renumber_sandbox_list(ed->pane);
            }

            // Keep the suggestion list in step with what is typed.
            //
            // Paths only: a port is four digits from a closed set and has
            // nothing to complete against. Empty text closes the list rather
            // than offering the whole workspace -- an empty row is how you add
            // a path you are about to type in full, and burying it under a
            // thousand candidates would make the common case the loud one.
            if (!ed->pane.numeric) {
                // By VIEW, not by copy. This runs on every keystroke including
                // pure navigation, and allocating a fresh std::string per key
                // just to compare it against one the picker already holds is
                // the kind of cost that shows up as typing lag long before it
                // shows up in a profile.
                std::string_view q;
                if (const auto* row = ed->pane.form.focused())
                    if (const auto* t = std::get_if<form::field::Text>(&row->value))
                        q = t->value;

                // Suggestions require an EDIT SESSION, not just a non-empty
                // row.
                //
                // Without the editing() term, arrowing onto an existing entry
                // popped the completion list open for a row the user had not
                // typed a character into -- and then stole their next Esc to
                // dismiss a list they never asked for, which reads as Esc
                // being swallowed. Completion is an affordance for the thing
                // you are WRITING; browsing past a value is not writing it.
                ed->pane.completing = ed->pane.form.editing() && !q.empty();
                if (ed->pane.completing && q != ed->pane.complete.query()) {
                    // Only when it actually CHANGED.
                    //
                    // The naive version (clear_query() then type()) invalidated
                    // the memo twice per key and reset the selection each time
                    // -- so the O(N x query) filter re-ran on every arrow press
                    // too, and the highlight jumped back to the top while you
                    // were moving through it. Guarding on inequality means a
                    // navigation key costs nothing and the filter runs exactly
                    // once per actual edit.
                    ed->pane.complete.clear_query();
                    ed->pane.complete.type(q);
                }
            }
            return Cmd::none();
        },

        [&](SandboxListClose&) -> Cmd {
            auto* ed = m.ui.panel.get<pn::SandboxList>();
            if (!ed) return Cmd::none();

            const auto row_id = ed->pane.row_id;
            const auto values = pn::read_sandbox_list(ed->pane);
            const bool ports  = ed->pane.numeric;

            // ascend() restores the stashed parent, which is the sandbox pane
            // descend() put there. Not close(): that would leave None and drop
            // the whole overlay stack, so editing a path list would exit
            // Settings entirely.
            //
            // Via detail::ascend, NOT a bare m.ui.panel.ascend(). The raw call
            // is [[nodiscard]] and its false branch means "no stashed parent",
            // which slot.hpp documents as the caller's cue to CLOSE. Honouring
            // the bool by returning early instead leaves this overlay on screen
            // with nothing beneath it, so Esc becomes a no-op and the pane
            // cannot be left at all -- a worse outcome than the warning that
            // prompted the change (PR #64). detail::ascend owns both halves:
            // close-on-no-parent, and the revalidation a restored snapshot
            // needs because the model may have moved since the descent.
            detail::ascend(m);

            auto* o = m.ui.panel.get<pn::Sandbox>();
            if (!o) return Cmd::none();

            // Write into the config, then rebuild the rows from it. The list
            // rows display a COUNT, so the parent row only changes by being
            // reprojected -- there is no value in the form to poke.
            auto cfg = pn::read_sandbox_form(o->pane.form, o->pane.working);
            if (ports) {
                cfg.allow_ports.clear();
                for (const auto& v : values) {
                    const auto n = std::strtoul(v.c_str(), nullptr, 10);
                    if (n > 0 && n <= 65535)
                        cfg.allow_ports.push_back(static_cast<std::uint16_t>(n));
                }
                std::sort(cfg.allow_ports.begin(), cfg.allow_ports.end());
                cfg.allow_ports.erase(
                    std::unique(cfg.allow_ports.begin(), cfg.allow_ports.end()),
                    cfg.allow_ports.end());
                // Adding a port is an unambiguous statement of intent, so it
                // sets the mode rather than sitting inert under `full` or
                // `none`. The row's help says this will happen.
                //
                // And the inverse: emptying the list under `ports` would be a
                // policy that denies everything, which is never what deleting
                // the last port meant -- fall back to the default rather than
                // leaving a trap.
                if (!cfg.allow_ports.empty())
                    cfg.net_mode = sandbox_cfg::NetMode::Ports;
                else if (cfg.net_mode == sandbox_cfg::NetMode::Ports)
                    cfg.net_mode = sandbox_cfg::NetMode::Full;
            } else if (row_id == pn::kSbReadPaths) {
                cfg.read_paths = values;
            } else if (row_id == pn::kSbWritePaths) {
                cfg.write_paths = values;
            } else if (row_id == pn::kSbDenyPaths) {
                cfg.deny_paths = values;
            }

            reproject(*o, cfg);
            // Autosave, same as any other edit in the pane -- a list commit is
            // not a special case, and requiring a second gesture after Esc
            // would make it one.
            return sandbox_update(m, msg::SandboxMsg{SandboxSave{}});
        },

    }, sm);
}

}  // namespace agentty::app::detail
