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
#include <maya/host/sources.hpp>   // maya::on_key / on_paste / on_resize

#include <string>
#include <string_view>

#include "agentty/domain/conversation.hpp"   // ImageContent
#include "agentty/domain/id.hpp"
#include "agentty/io/http.hpp"               // http::CancelTokenPtr
#include "agentty/runtime/msg.hpp"

// ── agentty's value types, as jaal sees them ──────────────────────────────
// jaal's Sendable walks a type's fields to prove a Msg is safe to hand to
// another thread (D7). Id<Tag> is a strong newtype around one std::string,
// but it has user-declared constructors, so it isn't an aggregate and jaal
// can't look inside — it refuses rather than guess:
//
//   it contains 'agentty::Id<agentty::ToolCallIdTag>', a class jaal can't
//   see inside; if it owns everything it holds, specialise
//   jaal::sendable_opt_in for it
//
// It does own everything it holds: one std::string, by value, no views and
// no pointers. So it is Sendable, and Frozen too — nothing reachable
// through a const Id can change.
//
// Declared HERE rather than in domain/id.hpp so the domain header stays
// free of jaal: the same rule maya follows (host/interop.hpp), and the same
// reason — a value type shouldn't know which runtime is carrying it.
template <class Tag>
inline constexpr bool jaal::sendable_opt_in<agentty::Id<Tag>> = true;
template <class Tag>
inline constexpr bool jaal::frozen_opt_in<agentty::Id<Tag>> = true;

// ImageContent owns its bytes (LazyBytes) and a shared base64 cell, and
// LazyBytes owns a content-addressed Source plus the bytes it resolves to.
// Moving either to another thread is safe: every lazily-filled slot is
// written exactly once under std::call_once and read through an acquire load
// after, so a thread either runs the fill or waits for it and then sees the
// finished bytes. (That discipline is not incidental — jaal's Sendable found
// a real race in LazyBytes on its first run against this code, 19 TSan
// reports to zero, and it is D7's worked example. The fix is what makes
// these opt-ins honest rather than silencers.)
//
// Sendable, NOT Frozen. Frozen means nothing reachable through a const T can
// change, and here the memoised bytes and the memoised base64 both can,
// behind const accessors. That is D8's distinction exactly — safe to MOVE to
// one other thread, not safe to SHARE between two — so these say Sendable
// only, and jaal::shared<ImageContent> stays correctly impossible.
template <>
inline constexpr bool jaal::sendable_opt_in<agentty::LazyBytes> = true;
template <>
inline constexpr bool jaal::sendable_opt_in<agentty::ImageContent> = true;

// nlohmann::json owns its whole tree by value (a variant over string, array,
// object, number, bool, null — every branch an owning container). jaal can't
// walk it because the payload is behind a private union, not because there
// is anything borrowed in there. Tool arguments and results are json, so
// they cross to worker threads constantly.
//
// Sendable, not Frozen: a json is freely mutable through a non-const
// reference, and nothing here pretends otherwise.
template <>
inline constexpr bool jaal::sendable_opt_in<nlohmann::json> = true;

// http::CancelToken is a shared mutable object, and Sendable refuses
// shared_ptr by default for exactly that reason. This is the case the rule
// is measured against rather than a hole in it: CancelToken is ONE
// std::atomic<bool> with a release store and an acquire load, and nothing
// else. Sharing it is the point — a reducer trips it (Esc in meta.cpp, a new
// turn in stream.cpp) while the tool worker polls it, and an atomic flag is
// how that conversation is supposed to happen.
//
// Not to be confused with the login flows, which carried their own
// shared_ptr<atomic_bool> for a job std::stop_token already did: those are
// gone (see device_login_async). The difference is ownership. A stop_token
// is tripped by the RUNTIME when the work is no longer subscribed; this flag
// is tripped by the PROGRAM, from a reducer, and is part of the Model's
// turn state. jaal has no effect for "cancel that specific in-flight HTTP
// request from a later reducer step", so this stays the app's own.
//
// Opting in the pointer, not the token: the shared_ptr is what crosses.
template <>
inline constexpr bool jaal::sendable_opt_in<agentty::http::CancelTokenPtr> = true;

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

/// Every event source an agentty subscription may name.
///
/// The mirror of the Cmd row, for the input side (jaal D2): a program says
/// which sources it listens to, and a host that can't produce one won't
/// compile against it. jaal's core sources — `every` and `stream` — are
/// always in and need no naming; these are maya's, the things only a
/// TERMINAL reports.
///
/// agentty names only the three it actually uses. It has no mouse handler and
/// no focus handler, and leaving them out of the row is not cosmetic: it is
/// the difference between "this program needs a mouse" and "this program
/// would work on a host that has none".
using Sub = jaal::Sub<Msg,
    maya::on_key,      // the composer and every panel's keymap
    maya::on_paste,    // bracketed paste, and the OSC 52 clipboard reply
    maya::on_resize    // relayout; the inline frame's width changed
>;

// ── spelling the terminal effects ──────────────────────────────────────
// jaal builds an effect by handing its PAYLOAD to the Cmd: `Cmd(SetTitle{s})`.
// That reads fine at one call site and poorly at forty, and the old runtime
// spelled these as named factories (`Cmd<Msg>::write_clipboard(s)`), so keep
// the names. They're free — each is one constructor call.
//
// These live in agentty::app::cmd, the same namespace as the app's own effect
// factories (runtime/app/cmd_factory.hpp): a reducer says `cmd::run_tool(...)`
// and `cmd::write_clipboard(...)` without caring which side of the seam an
// effect comes from. That is the point of a row — the program lists what it
// needs and the host's and the app's own effects sit together.
namespace app::cmd {

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

}  // namespace app::cmd

}  // namespace agentty
