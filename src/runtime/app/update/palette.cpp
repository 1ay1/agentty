// palette_update + todo_update — reducers for the command palette and
// the todo modal. Both are simple list-modals; the palette is the
// dispatcher for action commands (NewThread, ReviewChanges, etc.), so it
// re-enters the top-level update() to fan a Command::* into the matching
// domain Msg.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/view/palette.hpp"   // ui::palette_context

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

// Build the live visibility context for the palette from the current Model,
// so conditionally-dead rows (Accept-all with no diff, Run-code-block with no
// fenced reply, Update with no release) never render. One place, consulted by
// every filtered_commands() call in this reducer + the view.
// Defined in meta.cpp (declared in internal.hpp) so the view shares it.

// ── Command dispatch driver ───────────────────────────────────────────────
// Each palette Command declares ITS OWN behaviour in one place: the registry
// below is a flat table of {Command, handler}. Adding a command is one row
// here (plus its metadata row in command_palette.hpp) — no growing switch, no
// four-file edit. The vast majority just re-enter the reducer with a Msg, so
// `emit<T>()` builds that handler generically; the few commands with bespoke
// side effects (learning reset, self-update) get a named lambda.
//
// A handler is Model-in → Step-out; the driver has already closed the palette
// before calling it, so handlers only describe the action.
using CommandHandler = std::function<Cmd(Model&)>;

namespace {
// emit<T>: the handler for a command that simply dispatches Msg{T{}} back
// through the top-level reducer. Covers ~20 of the rows.
//
// Still goes through the whole-Msg reducer rather than a per-domain one:
// the commands span every domain, and routing each to its own reducer by
// hand is exactly the dispatch table this registry exists to avoid. The
// pair it returns is unpacked here, so the handlers themselves are already
// in jaal's shape.
template <class T>
[[nodiscard]] CommandHandler emit() {
    return [](Model& m) {
        auto [next, cmd] = agentty::app::update(std::move(m), Msg{T{}});
        m = std::move(next);
        return std::move(cmd);
    };
}
// emit_val<T>(v): same, for a Msg that carries a payload (settings category).
template <class T, class V>
[[nodiscard]] CommandHandler emit_val(V v) {
    return [v](Model& m) {
        auto [next, cmd] = agentty::app::update(std::move(m), Msg{T{v}});
        m = std::move(next);
        return std::move(cmd);
    };
}
// NOTE on Esc chains: the select arm below snapshots the palette and
// `adopt()`s it onto WHATEVER the command opened — generically, after
// dispatch. The old emit_settings<> wrapper (which hand-stamped an origin
// per settings command) is gone: commands need no per-caller knowledge,
// and every palette-opened overlay now unwinds back to the palette.
} // namespace

// The registry: Command → what it does. Ordered for readability, not lookup
// (the driver does a linear find over 25 entries — trivially cheap, and keeps
// the table declarative). One entry per Command in the enum; a missing entry
// is a no-op (handled by the driver), so the build can't silently mis-wire.
[[nodiscard]] const std::vector<std::pair<Command, CommandHandler>>& command_registry() {
    static const std::vector<std::pair<Command, CommandHandler>> reg = [] {
        std::vector<std::pair<Command, CommandHandler>> r;
        r.reserve(25);
        auto add = [&](Command c, CommandHandler h) { r.emplace_back(c, std::move(h)); };

        // ── Thread ──
        add(Command::NewThread,        emit<NewThread>());
        add(Command::ForkThread,       emit<OpenFork>());
        add(Command::CompactContext,   emit<CompactContext>());
        add(Command::RewindCheckpoint, emit<OpenCheckpoints>());
        // ── Changes ──
        add(Command::ReviewChanges,    emit<OpenDiffReview>());
        add(Command::AcceptAll,        emit<AcceptAllChanges>());
        add(Command::RejectAll,        emit<RejectAllChanges>());
        // ── Go ──
        add(Command::OpenThreads,      emit<OpenThreadList>());
        add(Command::OpenPlan,         emit<OpenTodoModal>());
        add(Command::InspectToolOutputs, emit<OpenToolOutput>());
        add(Command::RunCodeBlock,     emit<OpenCodeBlocks>());
        // ── Config ──
        add(Command::CycleProfile,     emit<CycleProfile>());
        add(Command::OpenModels,       emit<OpenModels>());
        add(Command::SwapModel,        emit<SwitchToPreviousModel>());
        add(Command::OpenProviders,    emit<OpenProviders>());
        add(Command::OpenStats,        emit<OpenStats>());
        add(Command::OpenSkills,       emit<OpenSkills>());
        add(Command::OpenGeneralSettings,
            emit_val<OpenSettingsList>(settings::Category::General));
        // ── Account ──
        add(Command::OpenLogin,        emit<OpenLogin>());
        // ── General ──
        add(Command::UpdateAgentty, [](Model& m) -> Cmd {
            if (m.s.update_latest.empty() || m.s.update_in_flight)
                return Cmd::none();
            m.s.update_in_flight = true;
            std::string v = m.s.update_latest;
            m.s.status = "\xe2\xac\x86 downloading agentty v" + v + "\xe2\x80\xa6";
            m.s.status_until = {};
            return cmd::perform_self_update(std::move(v));
        });
        add(Command::Quit,             emit<Quit>());
        return r;
    }();
    return reg;
}

// Run the command the cursor landed on. The palette is already closed by the
// caller; unknown commands (no registry entry) are a safe no-op.
[[nodiscard]] Cmd dispatch_command(Command sel, Model& m) {
    for (const auto& [id, handler] : command_registry())
        if (id == sel) return handler(m);
    return Cmd::none();
}

Cmd palette_update(Model& m, msg::PaletteMsg pm) {
    return std::visit(overload{
        [&](OpenPalette) -> Cmd {
            m.ui.panel.descend(pn::Palette{});
            return Cmd::none();
        },
        [&](ClosePalette) -> Cmd {
            // Esc unwinds one level — a palette opened over another panel
            // (rare but possible via chords) restores it; over the thread,
            // closes.
            ascend(m);
            return Cmd::none();
        },
        [&](PanelFilterPaste& e) -> Cmd {
            // ONE arm for every filter panel: replay the paste through the
            // open panel's OWN typed-input message, one char at a time — so
            // paste has exactly typing's semantics (ASCII gate, cursor
            // reset, the models panel's re-rank) with no duplicated logic
            // and no cross-TU coupling. Control chars are dropped here so a
            // multi-line clipboard can't smuggle newlines into a filter.
            // Bounded by clipboard size; each step is the cheap typed path.
            Cmd out = Cmd::none();
            for (char c : e.text) {
                const auto u = static_cast<unsigned char>(c);
                if (u < 0x20 || u >= 0x7f) continue;
                const auto ch = static_cast<char32_t>(u);
                Msg per_char =
                    m.ui.panel.is<pn::Palette>()   ? Msg{PaletteInput{ch}}
                  : m.ui.panel.is<pn::Models>()    ? Msg{ModelsFilterInput{ch}}
                  : m.ui.panel.is<pn::Providers>() ? Msg{ProvidersFilterInput{ch}}
                  : m.ui.panel.is<pn::Mention>()   ? Msg{MentionInput{ch}}
                  : m.ui.panel.is<pn::Symbol>()    ? Msg{SymbolInput{ch}}
                                                   : Msg{NoOp{}};
                auto [next, cmd] = agentty::app::update(std::move(m),
                                                        std::move(per_char));
                m = std::move(next);
                // Last writer wins, as before: the typed path's Cmds are
                // status toasts, and only the final keystroke's is current.
                out = std::move(cmd);
            }
            return out;
        },
        [&](PaletteInput& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (o && static_cast<uint32_t>(e.ch) < 0x80) {
                o->query.push_back(static_cast<char>(e.ch));
                // Reset cursor to the top of the (newly filtered) list so
                // the previous index doesn't point at a now-hidden row.
                o->index = 0;
            }
            return Cmd::none();
        },
        [&](PaletteBackspace) -> Cmd {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (o && !o->query.empty()) {
                o->query.pop_back();
                o->index = 0;
            }
            return Cmd::none();
        },
        [&](PaletteMove& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (!o) return Cmd::none();
            // Clamp against the *visible* row count, not kCommands.size().
            // Without the upper bound the cursor used to walk off-screen
            // and Enter would silently fall through to the no-match path.
            int sz = static_cast<int>(filtered_commands(
                o->query, ui::palette_context(m)).size());
            if (sz <= 0) { o->index = 0; return Cmd::none(); }
            o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            return Cmd::none();
        },
        [&](PaletteSelect) -> Cmd {
            auto* o = m.ui.panel.get<pn::Palette>();
            if (!o) return Cmd::none();
            // Resolve cursor → typed Command via the SAME filtered list
            // the view rendered. The previous design switched on the raw
            // o->index against the unfiltered enum, which silently fired
            // the wrong command whenever any query was active.
            auto matches = filtered_commands(o->query, ui::palette_context(m));
            // Copy out BEFORE closing: `o` points into the variant, and
            // close() destroys that alternative (the old code read o->index
            // through the dangling pointer afterwards).
            const int idx = o->index;
            if (matches.empty()
                || idx < 0
                || idx >= static_cast<int>(matches.size())) {
                m.ui.panel.close<pn::Palette>();
                return Cmd::none();
            }
            const Command sel = matches[static_cast<std::size_t>(idx)]->id;
            // Snapshot the palette — query, cursor, its own parent chain —
            // then dispatch and let WHATEVER the command opened adopt it as
            // its Esc target. Generic: the registry needs no per-command
            // origin plumbing, and a command that opens nothing leaves the
            // slot None, where adopt() is a no-op.
            auto parent = pn::From::of(pn::Snapshot{m.ui.panel.raw()});
            m.ui.panel.close<pn::Palette>();
            // Behaviour lives in the command registry (dispatch_command), not
            // an inline switch — one declarative table, no drift, and adding a
            // command never touches this arm.
            auto cmd = dispatch_command(sel, m);
            m.ui.panel.adopt(std::move(parent));
            return cmd;
        },
    }, pm);
}

Cmd todo_update(Model& m, msg::TodoMsg tm) {
    return std::visit(overload{
        [&](OpenTodoModal) -> Cmd {
            m.ui.todo.open = pick::OpenModal{};
            return Cmd::none();
        },
        [&](CloseTodoModal) -> Cmd {
            m.ui.todo.open = pick::Closed{};
            return Cmd::none();
        },
        // (No UpdateTodos arm: the agent's todo writes land via
        // stream_preview's direct sync — see sync_todos there. A message
        // nobody sent.)
    }, tm);
}

} // namespace agentty::app::detail
