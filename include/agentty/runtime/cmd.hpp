#pragma once
// agentty::Cmd — the effect row, declared once.
//
// jaal puts a program's effects IN ITS TYPE (D2): `Cmd<Msg, a, b, c>` says
// which effects this program may return, and a host must handle every one
// of them or the program won't compile against it. That is the property
// that makes the ACP host tractable — the compiler lists what it still owes
// instead of us finding out at runtime.
//
// So the row lives here, in one place, and everything else says `Cmd`.
//
// Three groups:
//
//   jaal core     quit, send, after, task, now, random — always available,
//                 no need to name them (D6: the core row is always in).
//   maya terminal set_title, commit_scrollback, write_clipboard, suspend,
//                 ... — things only a TERMINAL can do. A non-terminal host
//                 (ACP) will not serve these, which is exactly why they are
//                 listed: the compiler will say so.
//   agentty's own save_settings, save_thread, ... — things only THIS app
//                 does. Added as the Deps seam is dismantled (step 7 in
//                 docs/design/jaal-rewrite.md).

#include <jaal/jaal.hpp>
#include <maya/host/effects.hpp>

#include <string>
#include <string_view>

#include "agentty/runtime/msg.hpp"

namespace agentty {

/// Every effect an agentty reducer may return.
///
/// Add to this row when you add an effect; the host that can't run it stops
/// compiling, with `require_host_for` naming the effect and the program.
using Cmd = jaal::Cmd<Msg,
    // ── maya's terminal effects ──────────────────────────────────────
    maya::commit_scrollback,   // hand inline rows to the terminal's scrollback
    maya::write_clipboard,     // OSC 52 write
    maya::query_clipboard,     // OSC 52 read; reply arrives as a paste
    maya::emit_host_sequence,  // a formed control sequence (editor hooks)
    maya::reset_inline,        // drop the inline frame, start fresh below it
    maya::force_redraw,        // repaint from scratch
    maya::set_mouse,           // mouse reporting on/off
    maya::suspend              // hand the tty to a child (editor, pager)
>;

// ── spelling the terminal effects ──────────────────────────────────────
// jaal builds an effect by handing its PAYLOAD to the Cmd: `Cmd(SetTitle{s})`.
// That reads fine at one call site and poorly at forty, and the old runtime
// spelled these as named factories (`Cmd<Msg>::write_clipboard(s)`), so keep
// the names. They're free — each is one constructor call.
namespace cmd {

[[nodiscard]] inline Cmd write_clipboard(std::string text) {
    return Cmd(maya::WriteClipboard{std::move(text)});
}
[[nodiscard]] inline Cmd query_clipboard() {
    return Cmd(maya::QueryClipboard{});
}
[[nodiscard]] inline Cmd emit_osc(int code, std::string_view payload) {
    return Cmd(maya::osc(code, payload));
}
[[nodiscard]] inline Cmd emit_host_sequence(std::string seq) {
    return Cmd(maya::EmitHostSequence{std::move(seq)});
}
[[nodiscard]] inline Cmd reset_inline() {
    return Cmd(maya::ResetInline{});
}
[[nodiscard]] inline Cmd force_redraw() {
    return Cmd(maya::ForceRedraw{});
}
[[nodiscard]] inline Cmd set_mouse(bool on) {
    return Cmd(maya::SetMouse{on});
}

/// Commit a harvested scrollback debt; nothing to do when it's empty.
/// The typed ScrollbackDebt is the point — only maya's ledger can mint one,
/// so a reducer can't commit a row count that drifts from the wire.
[[nodiscard]] inline Cmd commit_scrollback(maya::ScrollbackDebt debt) {
    return maya::commit_from<Cmd>(debt);
}

}  // namespace cmd

}  // namespace agentty
