// agentty::tools::util::sandbox — the claybin backend.
//
// Same boundary as the bwrap backend, built through claybin's library API
// instead of by exec'ing a binary. Kept in its own translation unit so the
// bwrap path is untouched: bwrap has a decade of hardening and demonstrably
// works on users' machines, claybin is days old, and the choice between them
// belongs to the user at runtime rather than to whoever edits this file.
//
// WHY BOTH, concretely:
//   - no `bwrap` on PATH requirement, so a host without bubblewrap installed
//     still gets a sandbox.
//   - three walls bwrap gives us nothing for today, because we pass it no
//     --seccomp fd and no cgroup: a syscall filter, landlock as a second
//     filesystem wall, and real cgroup2 memory/pids caps.
//   - a guarantee report, so `describe_state()` can say WHICH walls are up
//     rather than the word "active".
//
// WHAT MUST NOT DIVERGE: the read set. kSystemReadRoots, kEtcReadable and
// kHomeToolSubdirs live in sandbox.cpp and are consumed here by reference --
// NOT restated. A previous backend restated them by hand and ended up granting
// read on `/`, which handed ~/.ssh and ~/.aws to any approved bash call while
// still reporting "sandbox: active". One list, two backends.
#include "agentty/tool/util/sandbox_claybin.hpp"

#if defined(AGENTTY_HAVE_CLAYBIN) && defined(__linux__)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "claybin/plan/compile.hpp"
#include "claybin/plan/spawn.hpp"
#include "claybin/policy/policy.hpp"
#include "claybin/policy/profiles.hpp"

namespace agentty::tools::util::sandbox::claybin_backend {

namespace {

using namespace ::clay;
using namespace ::clay::literals;

// Build the sealed policy that mirrors build_bwrap_argv()'s posture.
//
// Order matters exactly as it does for bwrap: the workspace bind goes LAST so
// that a workspace living under /tmp is not shadowed by the /tmp tmpfs mounted
// for it. claybin's mount plan is applied in declaration order for the same
// reason bubblewrap's is.
[[nodiscard]] Policy<Sealed> build_policy(const Posture& p) {
    auto d = Policy<Draft>{};

    // ── the read set, from the SHARED lists ───────────────────────────────
    for (const auto& root : p.system_read_roots) {
        // /usr and /bin must exist for a shell to run at all; the rest are
        // best-effort, matching --ro-bind-try.
        if (root == "/usr" || root == "/bin")
            d = std::move(d).ro_bind(root, root);
        else
            d = std::move(d).bind_try(root, root);
    }
    for (const auto& f : p.etc_readable) d = std::move(d).bind_try(f, f);
    for (const auto& h : p.home_tool_dirs) d = std::move(d).bind_try(h, h);
    // Extra grants from the pane. read_paths are read-only; write_paths are
    // separate because granting write is a different decision from granting
    // read, and one combined list would make the dangerous one the easy one.
    for (const auto& r : p.read_paths) d = std::move(d).bind_try(r, r);
    for (const auto& w : p.write_paths) d = std::move(d).bind_try(w, w, false);

    // ── secrets, masked with MOUNTS ──────────────────────────────────────
    //
    // A mount, not a landlock deny, and that distinction cost me an
    // afternoon. landlock has no negative rule: add_rule() with zero rights
    // is ENOMSG (measured on abi 10), and omitting the rule means the path
    // INHERITS its ancestor's grant. So "bind $HOME, deny $HOME/.aws" is not
    // expressible in landlock at all -- I wrote it, watched the rule reach
    // the plan, and read the credentials anyway.
    //
    // What DOES work is making the path not be the file: bind something empty
    // over it. tmpfs for a directory, an empty file for a file. The guest
    // sees an empty ~/.aws rather than a denied one, which is also the better
    // failure mode -- a tool that reads it gets no credentials instead of an
    // EACCES it might report as a bug.
    //
    // bind_try semantics: a secret the user does not have is not an error,
    // and mask() is optional by default for exactly that reason.
    for (const auto& m : p.masked) d = std::move(d).mask(m);

    // ── pseudo-filesystems ───────────────────────────────────────────
    // A fresh procfs shows only our own pid namespace. dev_fs binds the handful
    // of device nodes a program needs rather than mounting devtmpfs, so
    // /dev/mem and friends are ABSENT rather than present-and-denied.
    d = std::move(d).proc_fs("/proc");
    d = std::move(d).dev_fs("/dev");

    // ── /tmp, then the workspace ───────────────────────────────────────
    // A tmpfs so nothing leaks into the host /tmp, and SIZED: claybin enforces
    // it as a mount option, so a runaway build hits ENOSPC inside the sandbox
    // instead of filling the host's memory. bwrap cannot express this.
    d = std::move(d).tmpfs("/tmp", Bytes{p.tmp_bytes});
    if (!p.workspace.empty()) {
        d = std::move(d).bind(p.workspace, p.workspace);
        d = std::move(d).workdir(p.cwd.empty() ? p.workspace : p.cwd);
    }

    // ── network ─────────────────────────────────────────────────────────
    //
    // Three modes, and the middle one is the interesting one:
    //
    //   Full  a blanket grant. The report says `partial` for network
    //         isolation, because a shared netns is not a boundary -- which is
    //         honest rather than flattering.
    //   None  no grant at all, so claybin leaves the netns empty. `strong`.
    //   Ports one grant per port, enforced by landlock (abi 4+). `strong`,
    //         and the thing no proxy-based allowlist can claim: an agent
    //         cannot be talked into bypassing a kernel rule.
    switch (p.net_mode) {
        case 1: break;  // None
        case 2:
            for (auto port : p.allow_ports) d = std::move(d).connect("", port);
            break;
        default: d = std::move(d).connect("", 0); break;  // Full
    }

    // ── process hardening ──────────────────────────────────────────────
    // new_session: the child cannot steal the controlling tty (bwrap's
    //   --new-session, and why TIOCSTI injection is unreachable).
    // die_with_parent: no detached zombies if agentty dies.
    d = std::move(d).new_session();
    d = std::move(d).die_with_parent();

    // ── the walls bwrap is given none of ────────────────────────────────
    //
    // Off is everything(), NOT "no profile": a default-constructed
    // SyscallPolicy is kill-by-default with no rules, which compiles to a
    // sandbox that kills the guest at execve. claybin refuses it outright.
    // The settings pane's live preview caught that the first time it ran.
    switch (p.syscall_mode) {
        case 0: d = std::move(d).syscall_profile(SyscallPolicy::everything()); break;
        case 2: d = std::move(d).syscall_profile(profiles::with_filesystem()); break;
        default: d = std::move(d).syscall_profile(profiles::compiler_with_network()); break;
    }

    // Resource caps. cgroup2 when the host delegates, rlimit as a backstop;
    // the report distinguishes the two rather than claiming both.
    if (p.memory_bytes) d = std::move(d).memory(Bytes{p.memory_bytes});
    if (p.max_processes) d = std::move(d).processes(p.max_processes);
    if (p.cpu_percent) d = std::move(d).cpu_percent(p.cpu_percent);

    return std::move(d).seal();
}

}  // namespace

bool available() {
    // The same question the bwrap probe asks, and for the same reason: a
    // capability that reads as present but fails on use is worse than absence,
    // because Auto then degrades silently instead of loudly.
    //
    // claybin's probe_host() forks and attempts the uid_map write rather than
    // reading sysctls -- so this is a faithful predictor of spawn(), not a
    // guess about the kernel's feature list.
    auto host = probe_host();
    return host.user_namespaces && host.mount_namespaces;
}

Report describe(const Posture& p) {
    Report r;
    auto sealed = build_policy(p);
    auto compiled = compile(sealed, probe_host());
    if (!compiled) {
        r.ok = false;
        r.detail = std::string{compiled.error().mechanism};
        return r;
    }
    r.ok = true;
    for (int i = 0; i < static_cast<int>(CapId::count_); ++i) {
        auto id = static_cast<CapId>(i);
        auto s = compiled->guarantees.strength(id);
        Wall w;
        w.name = ::clay::cap_name(id);
        w.strength = s == Enforcement::strong    ? "strong"
                     : s == Enforcement::partial ? "partial"
                                                 : "none";
        r.walls.push_back(std::move(w));
    }
    return r;
}

SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd, int stdout_fd,
                        int stderr_fd) {
    SpawnResult out;

    auto sealed = build_policy(p);
    auto compiled = compile(sealed, probe_host());
    if (!compiled) {
        out.start_error = "claybin: compile failed: " + std::string{compiled.error().mechanism};
        return out;
    }

    // /bin/sh -c, matching the bwrap path: pipes, redirects and globs are part
    // of what a shell tool is for.
    const char* argv[] = {"/bin/sh", "-c", shell_cmd.c_str(), nullptr};
    Command cmd{"/bin/sh", argv, nullptr};
    // stdin from /dev/null explicitly rather than inherited -- a guest that can
    // read the user's terminal can prompt them, and nothing a tool runs should.
    cmd.stdin_fd = Command::kDevNull;
    cmd.stdout_fd = stdout_fd;
    cmd.stderr_fd = stderr_fd;

    auto spawned = spawn(compiled->plan, cmd);
    if (!spawned) {
        out.start_error = "claybin: spawn failed: " + std::string{spawned.error().mechanism} +
                          " (errno " + std::to_string(spawned.error().sys_errno) + ")";
        return out;
    }

    out.started = true;
    out.pid = spawned->pid;
    out.pidfd = spawned->pidfd;
    return out;
}

}  // namespace agentty::tools::util::sandbox::claybin_backend

#endif  // AGENTTY_HAVE_CLAYBIN && __linux__
