#pragma once
// agentty::tools::util — the ONE place agentty runs another program.
//
// mcp-cpp asks (mcp::tools::Exec); jaal supplies the primitive
// (jaal::platform::posix_process, whose exit is a handle a reactor watches).
// This is the piece in between: the policy.
//
// WHY THIS FILE EXISTS AT ALL
//
// There used to be two poll loops — one in mcp-cpp, one here — and they
// drifted. One grew an absolute wall-clock ceiling, the other kept only an
// idle one, and because agentty installed ITS runner into mcp-cpp whenever
// the sandbox was on, the configuration that shipped was the one without the
// ceiling. A command that never stopped printing was never reaped. The bug
// was not in either loop; it was in there being two.
//
// So there is one, it is here, and the things that used to be duplicated are
// now split by WHO KNOWS THEM:
//
//   jaal     how to spawn, how to wait, how an exit becomes readable.
//            Platform facts, one conformance suite, several backends.
//   here     how long to wait, what to do when that runs out, how much
//            output to keep. Policy, which differs per caller and belongs
//            to the app that has the user.
//   mcp-cpp  nothing. It states intent and reads bytes.
//
// THE TWO CLOCKS, which is the whole reason policy is interesting:
//
//   idle  bounds SILENCE. Reset by every byte, so a build that keeps
//         printing is never cut off however long it runs. This is the one
//         that catches a hang.
//   wall  bounds TOTAL TIME and never resets. This is the one that catches
//         a runaway that stays chatty — which the idle clock structurally
//         cannot, because output is exactly what keeps it alive.
//
// Neither alone is enough and that is not a tuning question, it is what the
// two measure. Tonight's bug was having only the first.

#include <chrono>
#include <memory>

#include <mcp/tools/host.hpp>

namespace agentty::tools::util {

/// Defaults applied when a request leaves a budget unset.
///
/// Generous on purpose. A ceiling that fires on real work teaches people to
/// raise it everywhere, and a limit everyone disables is not a limit. The
/// wall default is derived from the idle one rather than fixed, so a caller
/// that asks for a long silence implicitly asks for a long run.
///
/// `$AGENTTY_TOOL_HARD_TIMEOUT_SECS` overrides the wall default for the
/// process (0 disables the wall clock entirely). A ceiling nobody can raise
/// is its own kind of broken: the one session where a legitimate job needs
/// forty minutes should not need a rebuild.
struct ExecDefaults {
    std::chrono::seconds idle{120};
    /// wall = idle × this, floored at `wall_floor`.
    int                  wall_multiple = 20;
    std::chrono::seconds wall_floor{600};
    std::size_t          max_output_bytes = 8u * 1024u * 1024u;
    /// SIGTERM, then this long, then SIGKILL. Long enough for a shell to
    /// flush and reap its own children; short enough not to look hung.
    std::chrono::milliseconds kill_grace{2000};
};

/// Build the exec capability agentty hands to mcp-cpp.
///
/// Sandboxed when a sandbox is active. Two ways in, one way to watch:
/// claybin clones with namespace flags, installs a seccomp filter and enters
/// a cgroup -- none of which is expressible as "fork then exec this argv" --
/// so it spawns, and jaal adopts the pid and pidfd it hands back. Everything
/// after that line is identical whichever way the child started, including
/// the conformance suite that pins the exit semantics.
///
/// The seccomp broker, when the policy delegates syscalls, is serviced in
/// the SAME reactor wait as output and exit. The kernel blocks the guest
/// until someone answers a brokered call, so an unpolled listener is a hang
/// rather than a weaker wall -- and a wait that already covers three kinds
/// of readiness is the reason not to grow a thread for it.
[[nodiscard]] std::shared_ptr<::mcp::tools::Exec>
make_exec(ExecDefaults defaults = {});

}  // namespace agentty::tools::util
