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

// Linux-only, and that is claybin's own split rather than a build toggle:
// its plan compiler is portable (and the pane's preview uses it on any host),
// but only Linux can APPLY a plan -- namespaces, landlock, seccomp, cgroup2.
#if defined(__linux__)

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

        // Re-apply any mask that lives INSIDE the workspace, because the bind
        // above just remounted over it.
        //
        // This is an ordering bug I shipped and then measured: masks are
        // applied at line ~94, the workspace bind lands here, and a bind of
        // the workspace root covers every mount underneath it. So
        // `<workspace>/.env` was masked and then un-masked one line later,
        // and a planted .env came back readable IN FULL under both backends.
        // Every table-level check passed the whole time -- the mask really
        // was in the policy, it just lost a race with a later mount.
        //
        // Order is the only fix: a mask must come after every bind that could
        // cover it. The $HOME masks are unaffected (nothing rebinds $HOME),
        // which is why ~/.ssh was safe and .env was not.
        for (const auto& m : p.masked) {
            if (m.starts_with(p.workspace)) d = std::move(d).mask(m);
        }
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
    // Descriptors: a separate lever from processes, because a runaway that
    // leaks fds exhausts the host's file table without ever forking.
    if (p.max_open_files) d = std::move(d).open_files(p.max_open_files);
    // Two different ceilings, deliberately both available. cpu_time bounds
    // total compute, so a process that sleeps forever is untouched; wall_clock
    // bounds elapsed time, so one that blocks forever is not. Neither
    // subsumes the other, and the tool layer's own timeout is a third thing
    // (it can be ignored by a child that traps SIGTERM -- these cannot).
    if (p.cpu_secs)
        d = std::move(d).cpu_time(Nanos{std::uint64_t{p.cpu_secs} * 1'000'000'000ull});
    if (p.wall_clock_secs)
        d = std::move(d).wall_clock(Nanos{std::uint64_t{p.wall_clock_secs} * 1'000'000'000ull});

    // Hostname. Not containment -- the guest cannot escalate either way -- but
    // the real host name leaks into build output, test snapshots and anything
    // that shells out to `hostname`, which makes those non-reproducible and
    // mildly fingerprintable. claybin already defaults its PolicyData to
    // "sandbox"; this is the row that lets a user keep the real one when a
    // build genuinely depends on it.
    if (p.fake_hostname) d = std::move(d).hostname("sandbox");

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

    // NOT gated on user namespaces, and that is the whole reason claybin is
    // worth having as a second backend.
    //
    // This used to require `user_namespaces && mount_namespaces`, which made
    // claybin useless on exactly the host that needs it most. Ubuntu 24.04
    // ships an AppArmor profile that denies the uid_map write to unconfined
    // binaries, so bwrap fails with "setting up uid map: permission denied"
    // and agentty reported no backend at all (issue from KhazAkar). Copying
    // bwrap's precondition meant copying bwrap's failure.
    //
    // But landlock and seccomp are UNPRIVILEGED mechanisms. Neither needs a
    // namespace, and AppArmor's userns restriction does not touch them. So on
    // that host claybin still delivers, measured through compile():
    //
    //   filesystem.read/write/exec  strong   landlock
    //   syscall.filter              strong   seccomp-bpf
    //   resource.memory             strong   cgroup2 memory.max
    //   privilege.drop              strong   no_new_privs + empty bounding set
    //   network.isolation           none     (needs a netns)
    //   process.isolation           none     (needs userns + pidns)
    //
    // That is a real sandbox where bwrap has none: the filesystem boundary --
    // the one that keeps an approved command out of ~/.ssh -- survives intact.
    // Losing network and pid isolation is a genuine downgrade and the pane's
    // wall report says so per capability, which is the honest way to ship a
    // partial boundary.
    //
    // The floor is landlock OR seccomp. With neither there is nothing left to
    // enforce and claiming a sandbox would be the lie this whole subsystem is
    // built to avoid, so we decline and let the caller fall back to bwrap (or
    // to a loud failure under --sandbox on).
    return host.landlock_abi > 0 || host.seccomp;
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

#endif  // __linux__
