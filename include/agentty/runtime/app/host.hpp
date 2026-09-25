#pragma once
// agentty::app::Host — maya's terminal host, plus agentty's own effects.
//
// maya::run() builds a terminal_host and hands it to jaal. That host knows
// every TERMINAL effect (scrollback, clipboard, suspend, …) and nothing
// else, which is correct: writing a thread to disk is not a thing a
// terminal does.
//
// So agentty wraps it. Everything maya's host handles is inherited; the
// four persistence effects get a handle() here. jaal's HostFor concept
// checks the union at compile time — an effect in agentty's Cmd row with
// no handler anywhere fails the build and NAMES itself, rather than
// silently never running.
//
// Why a wrapper and not a fork of maya::run: the terminal half is maya's
// business and should keep evolving there. This only adds the half maya
// can't know about.

#include <filesystem>
#include <utility>

#include <maya/host/terminal.hpp>

#include "agentty/io/persistence.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/store_fx.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty::app {

/// The host agentty actually runs on.
///
/// Inherits maya::terminal_host's handle()/start_source() set and adds the
/// store effects. Public inheritance is what makes the inherited overloads
/// visible to jaal's `handles<H, D, Msg>` probe.
template <class P>
struct Host : maya::terminal_host<P> {
    using maya::terminal_host<P>::terminal_host;

    // Declaring handle() below HIDES every inherited overload — name lookup
    // stops at the first scope that has the name, so jaal's
    // `h.handle(CommitScrollback{...})` probe would fail and the build would
    // report maya's own effects as unrunnable. This puts them back in scope.
    using maya::terminal_host<P>::handle;

    // ── The host_context hooks ──────────────────────────────────────
    //
    // Nothing to write here — maya's attach()/on_ready() are templated on the
    // context type, so they match `host_context<Host>` and are inherited.
    //
    // That was not free. They used to take `host_context<terminal_host<P>>`
    // exactly, and jaal detects the hooks with
    // `requires { host.attach(cx); }` where cx is host_context<THIS host>.
    // A derived host therefore failed the test, the kernel skipped attach(),
    // the terminal's input was never registered with the reactor — and the
    // app came up accepting no keys, silently. Same shape as the init() bug
    // in program.hpp: an optional hook the runtime can't call reads as
    // "absent" rather than as an error. Fixed in maya; see the note there.

    // ── agentty's effects ────────────────────────────────────────────────
    // Each is one call into the persistence layer. They run on the loop
    // thread after the reducer returns; the ones that would block (settings)
    // are write-behind inside persistence itself, so none of these stall a
    // frame.
    //
    // The payloads were copied when the reducer built the effect, so there
    // is nothing here borrowing from a Model that has since moved on.
    void handle(SaveThread e)   { persistence::save_thread(e.thread); }
    void handle(DeleteThread e) { persistence::delete_thread(e.id); }
    void handle(SaveSettings e) { persistence::save_settings(e.settings); }

    void handle(WriteFile e) {
        // Best-effort, exactly as the Deps seam was: diff-review reject
        // rewrites the file with only the accepted hunks kept. An error is
        // surfaced by the next read (the pane shows the file unchanged), and
        // throwing out of an effect would take the loop down for something
        // the user can recover from.
        (void)tools::util::write_file(std::filesystem::path{e.path},
                                      e.contents);
    }
};

}  // namespace agentty::app
