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
#include "agentty/tool/util/sandbox_broker.hpp"
#include "agentty/util/logx.hpp"   // AGT_LOG — the broker audit trail

// POSIX, not Linux-only. claybin's plan compiler is portable, and as of the
// macOS backend there are now two platforms that can APPLY a policy:
//
//   Linux  — namespaces, landlock, seccomp, cgroup2  (claybin/plan/spawn.hpp)
//   macOS  — seatbelt + rlimits                      (claybin/macos/backend.hpp)
//
// `build_policy()` below is shared between them, which is the entire point of
// claybin's design: one description of the boundary, compiled per platform,
// with a guarantee report saying what each host could actually enforce. The
// two spawn paths differ because the mechanisms differ; the POLICY does not.
#if defined(__linux__) || defined(__APPLE__)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "claybin/plan/compile.hpp"
#include "claybin/policy/policy.hpp"
#include "claybin/policy/profiles.hpp"

#if defined(__linux__)
#include "claybin/broker/notify.hpp"
#include "claybin/plan/spawn.hpp"
#else
#include "claybin/macos/backend.hpp"
#endif

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

    // ── the walls bwrap is given none of ────────────────────────────
    //
    // The syscall filter, plus any brokered calls.
    //
    // Built as one SyscallPolicy and applied ONCE, because a draft does not
    // expose its syscall policy for amendment -- `syscall_profile()` replaces
    // wholesale. So the notify rules have to be layered on here, before the
    // policy is handed over, rather than added afterwards.
    SyscallPolicy sp = [&] {
        switch (p.syscall_mode) {
            // Off is everything(), NOT "no profile": a default-constructed
            // SyscallPolicy is kill-by-default with no rules, which compiles to
            // a sandbox that kills the guest at execve. claybin refuses it
            // outright, and the settings pane's live preview caught that the
            // first time it ran.
            case 0: return SyscallPolicy::everything();
            case 2: return profiles::with_filesystem();
            default: return profiles::compiler_with_network();
        }
    }();

    // Brokered syscalls: decided at runtime by a supervisor rather than by the
    // filter. Layered on AFTER the profile, because the profile sets a flat
    // action for these and brokering has to win.
    //
    // Only when the caller actually wired a supervisor. A policy that brokers
    // with nobody listening HANGS the guest -- the kernel blocks the thread
    // until someone answers -- so this is gated on `p.broker` rather than being
    // unconditional. Getting that backwards turns a hardening feature into a
    // deadlock on every ptrace.
    //
    // Also skipped under syscall_mode Off: everything() is "no filter", and
    // adding notify rules to it would mean brokering on a sandbox the user
    // asked not to filter at all.
    //
    // Linux-only: brokering is built on seccomp user-notify, which macOS has
    // no equivalent of. Compiled out rather than no-op'd at runtime, so a
    // future reader does not have to work out whether `p.broker` means
    // something different on darwin -- there, it means nothing at all, and the
    // guarantee report says `syscall.filter: none` to match.
#if defined(__linux__)
    if (p.broker && p.syscall_mode != 0) {
        for (auto nr : broker::brokered_syscalls()) sp.notify(nr);
    }
#endif
    d = std::move(d).syscall_profile(std::move(sp));

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

#if defined(__APPLE__)

// ─────────────────────────── macOS ────────────────────────────────────
//
// Shorter than the Linux path because seatbelt is one mechanism rather than
// four, and because there is no broker: macOS has no seccomp user-notify, so
// there is nothing to supervise and `SpawnResult::service_broker` stays empty.

bool available() {
    // probe_host() forks and actually calls sandbox_init() on a throwaway
    // profile rather than testing for the symbol. Same discipline as the Linux
    // probe and for the same reason: a capability that reads as present but
    // fails on use is worse than absence, because Auto then degrades silently
    // instead of loudly.
    //
    // In-process probing is not an option here. A seatbelt profile is
    // IRREVERSIBLE -- once entered, a process cannot leave it -- so a probe
    // that applied one would confine agentty itself.
    return ::clay::macos::probe_host().seatbelt;
}

Report describe(const Posture& p) {
    Report r;
    auto sealed = build_policy(p);
    auto compiled = ::clay::macos::compile(sealed, ::clay::macos::probe_host());
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
                     : s == Enforcement::advisory ? "advisory"
                                                  : "none";
        // The receipt, same as Linux: "filesystem.read strong via seatbelt" is
        // checkable in a way that "sandbox: active" is not. The mechanism
        // strings differ per platform precisely because they are the truth.
        w.mechanism = compiled->guarantees.mechanism(id);
        r.walls.push_back(std::move(w));
    }
    return r;
}

SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd, int stdout_fd,
                        int stderr_fd) {
    SpawnResult out;

    auto sealed = build_policy(p);
    ::clay::macos::Options opts;
    opts.profile_name = "agentty";
    auto compiled = ::clay::macos::compile(sealed, ::clay::macos::probe_host(), opts);
    if (!compiled) {
        out.start_error =
            "claybin: compile failed: " + std::string{compiled.error().mechanism};
        return out;
    }

    // /bin/sh -c, matching the bwrap and Linux-claybin paths: pipes, redirects
    // and globs are part of what an approved bash call means.
    const char* argv[] = {"/bin/sh", "-c", shell_cmd.c_str(), nullptr};
    ::clay::macos::SpawnRequest req;
    req.program = "/bin/sh";
    req.argv = argv;
    req.stdout_fd = stdout_fd;
    req.stderr_fd = stderr_fd;
    if (!p.cwd.empty())
        req.workdir = p.cwd;
    else if (!p.workspace.empty())
        req.workdir = p.workspace;

    auto spawned = ::clay::macos::spawn(*compiled, req);
    if (!spawned) {
        out.start_error = "claybin: spawn failed: " +
                          std::string{spawned.error().mechanism} + " (errno " +
                          std::to_string(spawned.error().sys_errno) + ")";
        return out;
    }

    out.started = true;
    out.pid = spawned->pid;
    // No pidfd on darwin and no broker to service: macOS has no seccomp
    // user-notify, so there is no supervisor fd. Left at their defaults rather
    // than faked, so the runner's `supervisor_fd >= 0` test does the right
    // thing without needing to know which platform it is on.
    return out;
}

#else  // __linux__

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
        // The receipt. claybin already knows this; not copying it out was the
        // difference between "the sandbox is active" and "filesystem.read is
        // strong via landlock abi 10", and only the second is checkable.
        w.mechanism = compiled->guarantees.mechanism(id);
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

    // The seccomp listener, when the policy brokers syscalls.
    //
    // claybin hands the listener to the CALLER, and the caller must answer
    // every notification: the kernel blocks the guest thread until someone
    // responds, so an unpolled listener is a hang rather than a weaker wall.
    // The subprocess runner polls `supervisor_fd` alongside the output pipe and
    // calls `service_broker` when it is readable.
    if (spawned->notify_fd >= 0) {
        out.supervisor_fd = spawned->notify_fd;

        // The listener lives in the closure, by shared_ptr, because
        // SpawnResult is copyable and a Listener is not. It closes when the
        // last copy of the callback dies, which is the right lifetime: the
        // runner drops the callback when the command ends.
        auto listener = std::make_shared<::clay::broker::Listener>(
            ::clay::OwnedFd{spawned->notify_fd});
        // The guest's own pid doubles as its process group: claybin calls
        // setsid() (new_session), so the child leads its own group and every
        // descendant inherits it. That is what makes "inside my own group" a
        // decidable question from a scalar.
        const std::uint32_t guest_pid = static_cast<std::uint32_t>(spawned->pid);

        out.service_broker = [listener, guest_pid]() -> bool {
            auto req = listener->next();
            if (!req) return false;   // guest gone, or the fd closed

            broker::Event ev;
            const auto verdict =
                broker::decide(req->nr, req->args, req->pid, guest_pid, &ev);

            // THE CHECK EVERY IMPLEMENTATION GETS WRONG, done immediately
            // before responding.
            //
            // A notification id can be reused once its thread is gone. If the
            // guest thread died between `next()` and here, responding would
            // answer a DIFFERENT syscall than the one we judged -- the kernel
            // offers no other way to close that race, which is why
            // seccomp_unotify(2) documents the ID_VALID ioctl as mandatory
            // rather than advisory.
            //
            // Returning true (not false) on a dead notification: the listener
            // itself is still fine, only this request is stale. Tearing down
            // the supervisor here would strand every later brokered call.
            if (!listener->still_valid(*req)) {
                AGT_LOG(Tool, Debug, "sandbox.broker",
                        "notification went stale before the response; dropped");
                return true;
            }

            const auto decision = verdict == broker::Verdict::Allow
                                      ? ::clay::broker::Decision::allow_it()
                                      : ::clay::broker::Decision::deny_it(EPERM);

            // Logged at every outcome, because this is the audit trail that
            // makes the sandbox teachable. "your build failed" says nothing;
            // "cargo tried ptrace ATTACH pid=1 and was denied" says what the
            // toolchain does and what to allow if you disagree.
            AGT_LOG(Tool, Info, "sandbox.broker", "{} {} {}",
                    ev.syscall, ev.detail, ev.allowed ? "allowed" : "DENIED");

            // And into the feed the settings pane reads. record() ignores
            // allows, so this is the denial path only -- the log is the trace,
            // the feed is the security surface.
            //
            // Called from a tool WORKER thread; the feed takes a lock for
            // exactly this reason.
            broker::record(ev);

            if (!listener->respond(*req, decision)) {
                // Responding failed, which usually means the guest died while
                // we were deciding. Not an error worth tearing down for; the
                // next next() will report the listener finished.
                AGT_LOG(Tool, Debug, "sandbox.broker", "respond failed");
            }
            return true;
        };
    }
    return out;
}

#endif  // __APPLE__ / __linux__

}  // namespace agentty::tools::util::sandbox::claybin_backend

#endif  // __linux__ || __APPLE__
