#include "agentty/tool/util/sandbox.hpp"

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/logx.hpp"   // AGT_LOG — the sealed-policy refusal

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>   // shared_ptr — the config snapshot

#include <maya/runtime.hpp>
#include <string>
#include <vector>

// The policy type is a PRODUCT concept, not a claybin one: a build without
// claybin still has a sandbox posture, it just cannot enforce most of it. So
// this include is unconditional and the enforcement is what is guarded --
// otherwise "what is my policy" would be a question only some builds could
// answer, and the settings pane has to work in both.
#include "agentty/domain/sandbox_config.hpp"

// claybin is a required submodule, so the only question left is PLATFORM: it
// compiles a plan anywhere, and can APPLY one on Linux (namespaces, landlock,
// seccomp, cgroup2) and on macOS (seatbelt, rlimits). Windows compiles the
// policy and refuses to apply it, so the include stays out there.
#if defined(__linux__) || defined(__APPLE__)
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/handoff_gate.hpp"   // snapshot_trusted: ONE path table
#endif

namespace agentty::tools::util::sandbox {

namespace fs = std::filesystem;

// Escape a path for safe interpolation into the macOS sandbox-exec (SBPL)
// profile string. The workspace root is user/attacker-influenceable (via
// --workspace / cwd) and macOS/APFS permits ", \, and control chars in
// directory names — interpolating such a path raw would terminate the
// (subpath "…") string early and let the remainder be reparsed as SBPL
// (profile rejection at best, an injected (allow …) clause at worst). Escaping
// \ and " makes the literal inert; a control char can't be represented in an
// SBPL string at all, so such a path yields empty → the caller omits the
// clause and fails closed. Exported (not anon-namespace) so it's unit-tested.
std::string sbpl_escape(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char c : path) {
        if (c == '\0' || c == '\n' || c == '\r' || c == '\t')
            return {};                       // uncleanable → fail closed
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

namespace {

// Single-process state. Set by init() at startup, read by every bash
// call afterwards. Atomics aren't strictly needed (set once, never
// flipped after main has handed off to maya), but they cost nothing
// and document the read-many lifecycle.
std::atomic<Mode>    g_mode{Mode::Auto};
std::atomic<Backend> g_backend{Backend::None};

// Which Linux backend to prefer when both could work.
//
// claybin is the default. The fallback below means this is a PREFERENCE and
// not a requirement -- a host that cannot build claybin's walls still gets
// bwrap, never None.
//
// It used to be bwrap, on the reasoning that a decade of upstream hardening
// beat days and that an upgrade should not silently move anyone's boundary.
// The measurement retired that argument: agentty's bwrap path emits no
// --seccomp, no cgroup limits and no landlock, so against claybin it is
// strictly weaker on syscall.filter (none vs strong), all three resource caps
// (none vs strong), filesystem.exec (binds vs landlock) and brokering (none vs

// The user's saved sandbox policy. Written ONCE, at startup, then frozen.
//
// Sealed rather than republishable, and that seal is the pane's security
// model -- see set_config() for the full reasoning. The short version: a
// boundary that can move mid-session only ever moves usefully in the
// weakening direction, the weakening is invisible and cannot be undone, and
// it makes the sandbox unauditable because two commands in one session get
// two different walls.
//
// This is the ONLY setting in agentty that behaves this way. Everything else
// in the settings store is live by design; a theme is judged by looking at
// it. The sandbox is judged by reading what it promises, so it trades
// immediacy for a property nothing else here needs.
//
// Still a published shared_ptr-to-const rather than a plain global: written
// once but READ from tool worker threads and the UI thread at the same time,
// and this is how a reader gets a coherent view without a lock.
//
// Defaults reproduce today's behaviour, so a user who never opens the pane
// gets exactly the sandbox they already had.
maya::published<const sandbox_cfg::Config> g_cfg;

// Has the policy been sealed? Set by the first set_config() call.
//
// A plain atomic bool rather than "g_cfg != nullptr", because the two are
// not the same question: sealing must latch even if someone seals with a
// default-constructed config, and conflating them would let a second call
// through whenever the first happened to publish something falsy.
std::atomic<bool> g_cfg_sealed{false};

// Only the POSIX backends (Linux bwrap, macOS sandbox-exec) need the
// "can we run this binary?" probe — the Windows/unsupported branch
// just hard-codes Backend::None.
#if defined(__linux__) || defined(__APPLE__)

// One-shot probe: try to spawn `<exe> --version` and observe the
// outcome. Replaces a fragile PATH walk with the actual semantic
// check we care about ("can we run this binary?"). The result is
// thrown away — exit code 0 just means the binary exists and starts.
[[nodiscard]] bool can_invoke(const char* exe) {
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Argv{{exe, "--version"}};
    opts.timeout = std::chrono::seconds{2};
    opts.max_bytes = 4096;
    auto r = Subprocess::run(std::move(opts));
    return r.started && r.exit_code == 0;
}

#endif // posix backends

#if defined(__linux__)

// A GENUINE minimal-sandbox probe. `bwrap --version` (the old check via
// can_invoke) only proved the BINARY EXISTS — it never creates a namespace, so
// it passed on hosts where bwrap is installed but unprivileged user namespaces
// are BLOCKED: Ubuntu 24.04's AppArmor `userns` restriction, RHEL/hardened
// `kernel.unprivileged_userns_clone=0` or `user.max_user_namespaces=0`, and
// many container/CI hosts. There every REAL command then died with
//     bwrap: setting up uid map: Permission denied
// while agentty happily reported "sandbox: active (bwrap)" — GitHub issue #21.
//
// So we run the SAME namespace unshares a real command uses against a trivial
// /bin/true and require it to actually start AND exit 0. This makes the probe
// a faithful predictor of run_wrapped(): if it can't build the namespace here,
// it can't build it for the shell either, and we report no backend so Auto
// degrades to unsandboxed and On surfaces a clear, actionable error instead of
// letting every command fail.

[[nodiscard]] Backend probe() {
    // One Linux engine. available() is a faithful predictor of spawn() --
    // probe_host() forks and attempts the uid_map write rather than reading
    // sysctls -- so "usable" means it will work, not that the kernel
    // advertises the feature.
    //
    // Its floor is landlock OR seccomp, neither of which needs a user
    // namespace. That is what makes one engine enough: the Ubuntu 24.04 case
    // that motivated a second backend (an AppArmor profile denying the
    // uid_map write, which killed bwrap outright) still gets the filesystem
    // boundary and the syscall filter here. The wall report says per
    // capability what a given host did not reach, so a reduced posture is
    // visible rather than silent.
#if defined(__linux__)
    if (claybin_backend::available()) return Backend::Claybin;
#endif
    return Backend::None;
}

// Build the bwrap argv prefix. Workspace gets read-write bound to
// itself; system dirs are bound read-only so the shell can find
// /bin/sh, libc, /etc/resolv.conf, etc.; /tmp is a fresh tmpfs (no
// leakage into / from the host /tmp); /proc and /dev are minimal;
// network namespace is shared (so `git push` / `npm install` / `curl`
// keep working — sandboxing those would break legitimate agent flows
// users expect to work).
//
// The intent is "bash can do anything WITHIN your workspace, plus
// network, plus read system libs — but it cannot mutate /etc,
// /home/<other-projects>, ~/.ssh, /opt, etc." That's the threat model
// we're actually defending against: a compromised or sloppy model
// running `rm -rf ~` or `cat ~/.ssh/id_rsa` after one user approval.
//
// `--die-with-parent` ensures the sandbox dies if agentty dies — no
// detached zombies. `--unshare-pid` gives the child a clean PID
// namespace so kills work cleanly. `--new-session` so the child can't
// steal the controlling tty.
// ---------------------------------------------------------------------------
// THE READ SET.
//
// Named lists rather than an inline wall of --ro-bind-try, because this is the
// security boundary and it should be readable as one. Each entry is here for a
// stated reason; anything not listed is NOT readable from inside the sandbox.
//
// Kept as data rather than open-coded because a second backend once restated
// this set by hand and got it wrong -- granting read on "/", which handed
// ~/.ssh, ~/.aws and ~/.config to any approved `bash` call that also had the
// network. Read + exfiltrate, exactly what the sandbox exists to close, and
// invisible because it still reported "sandbox: active". One list, so a future
// backend extends it instead of paraphrasing it.
//
// Visible to BOTH platform arms, because the claybin backend consumes them on
// Linux and on macOS alike. That is the point of the list: one boundary
// definition, extended rather than paraphrased, however many backends read it.
#endif  // __linux__ (reopened below)

#if defined(__linux__) || defined(__APPLE__)

// System roots: the toolchain and shared libraries. Read-only, and no /etc —
// see kEtcReadable.
constexpr const char* kSystemReadRoots[] = {
    "/usr", "/bin", "/lib", "/lib64", "/sbin", "/opt",
};

// /etc: the specific files a shell + toolchain + name resolution need, NOT the
// tree. Binding all of /etc exposed host secrets (/etc/shadow where readable,
// krb5 keytabs, corporate config) to a command that also has the network.
constexpr const char* kEtcReadable[] = {
    "/etc/resolv.conf",   // DNS
    "/etc/hosts",
    "/etc/nsswitch.conf", // NSS resolution order
    "/etc/host.conf",
    "/etc/passwd",        // uid->name (git, shells)
    "/etc/group",
    "/etc/localtime",     // timestamps
    "/etc/ssl",           // TLS trust store (curl/git over https)
    "/etc/pki",           // RHEL/Fedora trust store
    "/etc/ca-certificates",
    "/etc/ca-certificates.conf",
    "/etc/gitconfig",     // system git config
    "/etc/profile",
    "/etc/alternatives",  // Debian toolchain symlinks
};

// User-local toolchains (GitHub issue #21). Many toolchains install OUTSIDE
// /usr — webinstall.dev drops go/gofmt/node under ~/.local/opt and links them
// into ~/.local/bin; rustup/cargo, go, nvm, pyenv, rbenv, asdf, bun, deno and
// sdkman all live under $HOME. Without these an "approved" bash call could not
// find the very tools the user asked the agent to run.
//
// Deliberately EXCLUDES broad dirs that mix in secrets/app-data: ~/.local/share
// (app data), ~/.npm (may cache an _auth token), ~/.config (creds),
// ~/.local/state (logs/history) — and of course ~/.ssh and ~/.aws.
constexpr const char* kHomeToolSubdirs[] = {
    "/.local/bin", "/.local/opt", "/.local/lib", // webinstall.dev etc.
    // webinstall puts some packages under ~/.local/xbin rather than bin --
    // same tool, different directory, and a tool the agent cannot find is
    // indistinguishable from a broken sandbox from the user's side.
    "/.local/xbin",
    "/.cargo/bin", "/.rustup",   // Rust (bin only from .cargo)
    "/go/bin", "/.go",           // Go (GOPATH bin + webinstall)
    "/.nvm",                     // Node version manager
    "/.pyenv", "/.rbenv", "/.asdf", // version managers
    "/.bun/bin", "/.deno/bin",   // Bun / Deno
    "/.dotnet", "/.sdkman/candidates", // .NET / JVM
};
#endif  // __linux__ || __APPLE__

// Back into the per-platform arm the shared read set interrupted.
#if defined(__linux__)


// The read set, handed to claybin as DATA rather than restated in its backend.
//
// This function is what keeps the two backends honest. Both consume
// kSystemReadRoots / kEtcReadable / kHomeToolSubdirs, so a change to the
// boundary applies to both or neither. A previous backend restated the set by
// hand and ended up granting read on `/` -- handing ~/.ssh and ~/.aws to any
// approved bash call while still reporting "sandbox: active".
//
// POSIX-wide rather than Linux-only: the macOS claybin backend consumes the
// very same Posture. That is the payoff for describing the boundary as data
// instead of as argv -- the read set, the masks and the pane's extra grants
// cross the platform line unchanged, and only the mechanisms that enforce
// them differ.
//
// Hence the #endif immediately below, closing the Linux arm that opened far
// above: this one function has to be visible to BOTH the Linux and the macOS
// run_wrapped(), and the per-platform arm reopens right after it.
#endif  // __linux__ (reopened below)

#if defined(__linux__) || defined(__APPLE__)
[[nodiscard]] claybin_backend::Posture build_claybin_posture() {
    claybin_backend::Posture p;

    // The user's saved policy, or the shipped default when they have never
    // opened the pane. `configured` is what keeps an upgrade from changing
    // anyone's boundary.
    //
    // ONE snapshot, held for the whole function. This runs on a tool worker
    // thread while the reducer thread may be publishing a new policy, so
    // re-reading the global per field could mix two policies into one
    // sandbox -- read paths from the old one, syscall profile from the new.
    // Holding the snapshot also means a save cannot change the boundary of a
    // command that is already being built.
    const auto snap = config_snapshot();
    const auto& cfg = *snap;

    // ── the read set, by scope ────────────────────────────────────────────
    //
    // Minimal deliberately omits the $HOME toolchain dirs: for a
    // self-contained repo, anything under $HOME is suspicious. HostReadable
    // is the convenient answer other agents take, named honestly -- it is
    // still a mount namespace and still read-only outside the workspace, but
    // your other projects are readable by an approved command.
    for (const char* r : kSystemReadRoots) p.system_read_roots.emplace_back(r);
    for (const char* f : kEtcReadable) p.etc_readable.emplace_back(f);
    if (cfg.fs_scope == sandbox_cfg::FsScope::Toolchain) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            for (const char* sub : kHomeToolSubdirs)
                p.home_tool_dirs.emplace_back(std::string{home} + sub);
        }
    } else if (cfg.fs_scope == sandbox_cfg::FsScope::HostReadable) {
        p.system_read_roots.emplace_back("/");
    }
    // Whatever the user added on top.
    for (const auto& r : cfg.read_paths) p.read_paths.push_back(r);
    for (const auto& w : cfg.write_paths) p.write_paths.push_back(w);

    p.workspace = workspace_root().string();
    p.cwd = p.workspace;

    // ── secrets, always, plus the user's own masks ────────────────────
    //
    // kAlwaysMasked is not configurable, and that is the point: a control the
    // user can switch off to make their build work is a control that is off.
    // The pane can ADD masks, never remove these.
    //
    // The LIST is shared with the bwrap path (sandbox_cfg::mask_paths), so the
    // two backends cannot disagree about what is covered. Only the mechanism
    // is per-backend: MountKind::mask here, mount arguments there. It used to
    // be computed twice, and the copies drifted -- bwrap's didn't exist at all.
    {
        const char* home = std::getenv("HOME");
        auto masks = sandbox_cfg::mask_paths(cfg, p.workspace,
                                            home ? home : "");
        p.masked.insert(p.masked.end(),
                        std::make_move_iterator(masks.begin()),
                        std::make_move_iterator(masks.end()));
    }

    // ── /dev/tty: the controlling terminal is agentty's own screen ───────
    //
    // Not a credential, so it is not in kAlwaysMasked (that list is $HOME
    // secrets) -- but it is masked for the same reason, on every backend.
    //
    // claybin's devtmpfs binds the HOST /dev/tty into the sandbox, following
    // bubblewrap's node list exactly. That node resolves to the calling
    // process's controlling terminal, so a child that opens it writes
    // straight to the screen -- bypassing the pipe whose bytes clean_capture()
    // scrubs. `printf '\033[2J\033[3J' > /dev/tty` wipes the transcript.
    //
    // Masking is the right mechanism rather than dropping the node: an
    // absent /dev/tty changes behaviour for programs that probe for a
    // terminal, while a masked one still opens and still accepts writes --
    // they just go nowhere. Captured stdout/stderr are untouched.
    p.masked.emplace_back("/dev/tty");

    // ── trust-handoff prevention: bind host-trusted paths read-only ─────
    //
    // The PREVENTION half of the gate. Detection (handoff::review_trusted)
    // reports a write after the bytes land and works on every host; this stops
    // the write, and needs a mount namespace.
    //
    // Reuses handoff::snapshot_trusted so the two halves cannot disagree about
    // WHICH paths are host-trusted -- the same reasoning the mask list above
    // records for bwrap vs claybin, where two copies drifted and one of them
    // did not exist. One table, two mechanisms.
    //
    // Only EXISTING paths: a bind of a path that is not there fails, and
    // bind_try tolerating that is not the same as wanting it. A trusted path
    // CREATED during the session is what detection is for.
    //
    // Skipped when the policy is Allow -- a user who turned the gate off has
    // said these files are theirs to edit, and silently making them read-only
    // would be the config lying to them.
    if (cfg.handoff != sandbox_cfg::HandoffPolicy::Allow && !p.workspace.empty()) {
        auto snap = handoff::snapshot_trusted(p.workspace);
        p.handoff_ro.reserve(snap.entries.size());
        for (auto& e : snap.entries)
            if (e.existed) p.handoff_ro.push_back(std::move(e.path));
    }

    // ── network ──────────────────────────────────────────────────────────
    p.net_mode = static_cast<int>(cfg.net_mode);
    p.allow_ports = cfg.allow_ports;

    // ── syscalls and caps ────────────────────────────────────────────────
    p.syscall_mode = static_cast<int>(cfg.syscall_mode);
    p.wx_protect = cfg.wx_protect;
    p.scope_ipc = cfg.scope_ipc;
    p.close_inherited_fds = cfg.close_inherited_fds;
    p.tmp_bytes = cfg.tmp_mb * 1024ull * 1024;
    p.memory_bytes = cfg.memory_mb * 1024ull * 1024;
    p.max_processes = cfg.max_procs;
    p.cpu_percent = cfg.cpu_percent;
    p.max_open_files = cfg.max_open_files;
    p.wall_clock_secs = cfg.wall_clock_secs;
    p.cpu_secs = cfg.cpu_secs;
    p.fake_hostname = cfg.fake_hostname;
    // Brokering is on whenever the filter is, because the runner can always
    // poll the listener -- it already owns a poll loop for the output pipe.
    // Gated on the filter rather than being a separate row: brokering IS the
    // syscall filter deciding at runtime instead of up front, so a user who
    // turned the filter off has already said they do not want either.
    p.broker = cfg.syscall_mode != sandbox_cfg::SyscallMode::Off;
    return p;
}
#endif  // __linux__ || __APPLE__

// Back into the per-platform arm the posture builder interrupted.
#if defined(__linux__)

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;

#if defined(__linux__) || defined(__APPLE__)
    if (detected_backend() == Backend::Claybin) {
        // claybin is a library, so there is no argv prefix to build. It forks,
        // applies the plan, and execs itself -- but only the SPAWN; the
        // supervise loop (poll, progress, idle deadline, SIGTERM/SIGKILL, reap)
        // stays in Subprocess::run, shared with every other path.
        //
        // Identical on both platforms, which is the whole return on keeping
        // the supervise loop out of the backend: Linux hands back a pidfd and
        // a seccomp-notify fd, macOS hands back neither, and the runner needs
        // no branch to tell them apart -- the sentinel values already say so.
        //
        // `command` still carries the shell string so the runner's logging and
        // error messages read the same for both backends.
        opts.command = SubprocessOptions::Shell{std::string{cmd}};
        std::string shell_cmd{cmd};
        opts.spawner = [shell_cmd](const SubprocessOptions&,
                                   int pipe_write_fd) -> SubprocessOptions::SpawnedChild {
            SubprocessOptions::SpawnedChild out;
            auto posture = build_claybin_posture();
            // Both streams onto the one pipe the runner already made.
            auto r = claybin_backend::spawn_shell(posture, shell_cmd, pipe_write_fd,
                                                  pipe_write_fd);
            if (!r.started) {
                out.error = r.start_error;
                return out;
            }
            out.pid   = r.pid;
            out.pidfd = r.pidfd;   // jaal reaps by it: no pid-reuse race
            // Hand the syscall supervisor to the runner, which polls it beside
            // the output pipe. It must be polled for the child's whole life: a
            // brokered syscall blocks in the kernel until someone answers.
            out.supervisor_fd = r.supervisor_fd;
            out.service = std::move(r.service_broker);
            // Empty unless the posture actually got a cgroup with
            // cgroup.kill. The runner falls back to the process group then,
            // which is weaker (setsid escapes it) but is all there is.
            out.kill_tree = std::move(r.kill_tree);
            return out;
        };
        return Subprocess::run(std::move(opts));
    }
#endif

    // No backend: run_wrapped is only reached when the sandbox is active,
    // and on Linux "active" means claybin. Reaching here would mean the
    // probe said yes and the spawn path disagreed, so refuse rather than
    // run the command unconfined -- a command that was approved on the
    // understanding it would be boxed must not quietly escape the box.
    SubprocessResult unconfined;
    unconfined.started     = false;
    unconfined.start_error = "sandbox is active but no backend could wrap "
                             "this command; refusing to run it unconfined";
    return unconfined;
}

// argv-form: same wrap as the shell form, with the user's argv reaching the
// child exactly as given -- no `sh -c` indirection to re-parse quotes or
// `$vars`.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }

    SubprocessOptions opts;
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;

#if defined(__linux__)
    if (detected_backend() == Backend::Claybin) {
        opts.command = SubprocessOptions::Argv{user_argv};
        std::vector<std::string> argv = user_argv;
        opts.spawner = [argv](const SubprocessOptions&,
                              int pipe_write_fd) -> SubprocessOptions::SpawnedChild {
            SubprocessOptions::SpawnedChild out;
            auto posture = build_claybin_posture();
            auto r = claybin_backend::spawn_argv(posture, argv, pipe_write_fd,
                                                 pipe_write_fd);
            if (!r.started) { out.error = r.start_error; return out; }
            out.pid           = r.pid;
            out.pidfd         = r.pidfd;
            out.supervisor_fd = r.supervisor_fd;
            out.service       = std::move(r.service_broker);
            out.kill_tree     = std::move(r.kill_tree);
            return out;
        };
        return Subprocess::run(std::move(opts));
    }
#endif

    SubprocessResult unconfined;
    unconfined.started     = false;
    unconfined.start_error = "sandbox is active but no backend could wrap "
                             "this command; refusing to run it unconfined";
    return unconfined;
}

#elif defined(__APPLE__)

[[nodiscard]] Backend probe() {
    // claybin first, when asked for AND usable.
    //
    // Both engines here are seatbelt -- claybin calls sandbox_init() directly
    // from a compiled policy, sandbox-exec is Apple's CLI wrapper around the
    // same kernel mechanism -- so this is not a choice about strength. It is a
    // choice about what survives the trip.
    //
    // build_profile() below concatenates a fixed profile string, so it can
    // express exactly what it was written to express: the workspace, /tmp, the
    // caches. The claybin path compiles the same Posture the Linux backend
    // consumes, so the pane's read_paths, write_paths, masks and resource caps
    // mean something on a mac instead of being Linux-only settings that
    // silently do nothing here. It also produces the guarantee report, which
    // is the part worth having: "filesystem.read strong via seatbelt" is
    // checkable in a way that "sandbox: active" is not.
    //
    // The preference is shared with Linux rather than given a second knob. A
    // user's answer to "which engine applies my policy" does not change when
    // they move between a mac and a Linux box.
    //
    // claybin first where it can build, sandbox-exec otherwise. Both are
    // real boundaries here; the order just prefers the one whose policy
    // vocabulary the pane actually speaks.
    if (claybin_backend::available()) return Backend::Claybin;
    if (can_invoke("sandbox-exec")) return Backend::SandboxExec;
    // And the other direction, for symmetry with the Ubuntu 24.04 case: if
    // sandbox-exec is somehow missing (SIP damage, a stripped image), claybin
    // still talks to the kernel directly and does not need the binary.
    if (claybin_backend::available()) return Backend::Claybin;
    return Backend::None;
}

// Generate a minimal sandbox-exec profile. Allows reads broadly,
// limits writes to workspace + tmp + system caches, allows network
// (same rationale as bwrap: agent-typical commands need it).
//
// Apple deprecated `sandbox-exec` in public docs but the binary keeps
// working. The profile language is Scheme-ish; we keep it small so a
// future Apple removal is easy to spot.
[[nodiscard]] std::string build_profile(std::string_view workspace) {
    std::string p;
    p += "(version 1)\n";
    p += "(deny default)\n";
    // Process / signals
    p += "(allow process-exec)\n";
    p += "(allow process-fork)\n";
    p += "(allow signal (target same-sandbox))\n";
    // Reads: broad — same rationale as bwrap's ro-bind on system dirs.
    p += "(allow file-read*)\n";
    // Writes: workspace + tmp/cache regions only. The workspace path is
    // SBPL-escaped (see sbpl_escape). If it can't be represented safely (a
    // control char), the workspace write clause is OMITTED entirely rather
    // than risk a broken/injected profile — sandboxed bash then gets no
    // workspace write access, which is safe (fail closed), not a bypass.
    if (std::string ws = sbpl_escape(workspace); !ws.empty())
        p += "(allow file-write* (subpath \"" + ws + "\"))\n";
    p += "(allow file-write* (subpath \"/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/tmp\"))\n";
    p += "(allow file-write* (subpath \"/private/var/folders\"))\n";   // user caches
    p += "(allow file-write* (subpath \"/dev/null\"))\n";
    // /dev/tty is deliberately NOT allowed — see the long note on the bwrap
    // backend's --dev-bind /dev/null /dev/tty. It is the controlling
    // terminal, i.e. agentty's own screen, and it bypasses the pipe that
    // clean_capture() scrubs. A child writing \033[2J\033[3J there wipes the
    // transcript. Captured stdout/stderr are unaffected; this only removes a
    // child's ability to paint the UI directly.
    // Network: open. Restricting would break git push / curl / npm.
    p += "(allow network*)\n";
    p += "(allow system-socket)\n";
    p += "(allow mach-lookup)\n";
    p += "(allow iokit-open)\n";
    p += "(allow sysctl-read)\n";
    return p;
}

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;

    if (detected_backend() == Backend::Claybin) {
        // Byte-for-byte the Linux claybin branch minus the broker, which macOS
        // has no equivalent of. claybin forks, enters the sandbox and execs;
        // the supervise loop (poll, progress, idle deadline, TERM/KILL, reap)
        // stays in Subprocess::run, shared with every other path on every
        // platform.
        opts.command = SubprocessOptions::Shell{std::string{cmd}};
        std::string shell_cmd{cmd};
        opts.spawner = [shell_cmd](const SubprocessOptions&,
                                   int pipe_write_fd) -> SubprocessOptions::SpawnedChild {
            SubprocessOptions::SpawnedChild out;
            auto posture = build_claybin_posture();
            // Both streams onto the one pipe the runner already made, which is
            // what the sandbox-exec path gets from its file_actions.
            auto r = claybin_backend::spawn_shell(posture, shell_cmd, pipe_write_fd,
                                                  pipe_write_fd);
            if (!r.started) {
                out.error = r.start_error;
                return out;
            }
            out.pid = r.pid;
            // No supervisor fd and no broker: seccomp user-notify has no macOS
            // counterpart, so these keep their sentinel defaults and the
            // runner's `supervisor_fd >= 0` test skips the poll on its own.
            return out;
        };
        return Subprocess::run(std::move(opts));
    }

    auto profile = build_profile(workspace_root().string());
    opts.command = SubprocessOptions::Argv{{
        "sandbox-exec", "-p", std::move(profile),
        "/bin/sh", "-c", std::string{cmd}
    }};
    return Subprocess::run(std::move(opts));
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    SubprocessOptions opts;
    auto profile = build_profile(workspace_root().string());
    std::vector<std::string> argv{"sandbox-exec", "-p", std::move(profile)};
    for (const auto& a : user_argv) argv.push_back(a);
    opts.command = SubprocessOptions::Argv{std::move(argv)};
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    return Subprocess::run(std::move(opts));
}

#else // Windows / unsupported

[[nodiscard]] Backend probe() { return Backend::None; }

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    // Should never be called — is_active() returns false on this
    // platform — but defensively fall through to the unsandboxed path.
    return run_command_s(std::string{cmd}, max_bytes, timeout);
}

[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    return run_argv_s(user_argv, max_bytes, timeout);
}

#endif

} // namespace

bool init(Mode requested) {
    g_mode.store(requested, std::memory_order_release);
    auto found = (requested == Mode::Off) ? Backend::None : probe();
    g_backend.store(found, std::memory_order_release);
    if (requested == Mode::On && found == Backend::None) {
        // Strict mode + no backend = init failure. Caller decides
        // whether to abort startup or downgrade silently.
        return false;
    }
    return true;
}

Mode    requested_mode()   noexcept { return g_mode.load(std::memory_order_acquire); }
Backend detected_backend() noexcept { return g_backend.load(std::memory_order_acquire); }


void set_config(const sandbox_cfg::Config& cfg) {
    // SEALED AFTER THE FIRST CALL. A later call is ignored, and says so.
    //
    // This is the whole security model of the pane, so it is worth stating
    // plainly. Every other setting in agentty is live: change the theme and
    // the next frame is painted with it. A sandbox cannot work that way,
    // because the thing it configures is a BOUNDARY, and a boundary that can
    // move while the process runs has three problems no amount of care fixes:
    //
    //   1. It only ever gets WEAKER usefully. Tightening mid-session is
    //      fine but pointless (whatever already ran, ran); loosening is what
    //      an attacker wants, and a prompt-injected agent that can reach the
    //      settings reducer is exactly who would want it.
    //
    //   2. The weakening is INVISIBLE and IRREVERSIBLE. Switching claybin ->
    //      bwrap silently drops the seccomp filter, the cgroup caps AND the
    //      non-configurable secret masks -- measured: the same workspace
    //      .env reads back as zero bytes under claybin and `SECRET=leaked`
    //      under bwrap. Re-tightening afterwards does not un-read a key that
    //      already left.
    //
    //   3. Two commands in one session get two different boundaries, with
    //      nothing in the transcript saying which got which. That makes the
    //      sandbox unauditable, which is worse than a weaker sandbox that is
    //      at least one known thing.
    //
    // So the policy is fixed at startup, from the saved settings plus the
    // CLI, and the pane WRITES TO DISK for the next launch rather than
    // mutating the live one. Restart is the apply step. That is a real cost
    // -- you cannot try a tighter profile without relaunching -- and it buys
    // the only property that matters here: whatever confined the first
    // command in this session confines the last one too.
    //
    // Enforced in code, not by comment. It was a comment before ("call
    // before init()") and the pane broke it within a day of existing.
    if (g_cfg_sealed.exchange(true, std::memory_order_acq_rel)) {
        AGT_LOG(Tool, Warn, "sandbox.seal",
                "refused a live sandbox policy change: the boundary is sealed "
                "at startup and only a restart applies a new one");
        return;
    }

    // First call: publish the one snapshot this process will ever use.
    //
    // Still an immutable published snapshot rather than a plain global. The seal means it is written once, but it is READ from
    // tool worker threads while the UI thread reads it too, and
    // shared_ptr-to-const is how a reader gets a coherent view without a
    // lock. (It also kept a real use-after-free out of the tree back when
    // this was republishable: Config holds three vectors, and assigning one
    // under a concurrent reader is a freed-buffer walk.)
    auto next = std::make_shared<const sandbox_cfg::Config>(cfg);
    g_cfg.publish(std::move(next));

    // The policy carries the ENGINE, so sealing it seals the backend too.
    //
    // Only when the user has actually chosen: an unconfigured config holds
}

bool config_sealed() noexcept {
    return g_cfg_sealed.load(std::memory_order_acquire);
}

void reset_config_for_test() noexcept {
    // Order matters even here: drop the config first, then the seal, so a
    // concurrent reader never sees "unsealed but still holding the old
    // policy" -- which is the one state that would let a second set_config
    // land while a reader is mid-snapshot.
    (void)g_cfg.take();
    g_cfg_sealed.store(false, std::memory_order_release);
}

std::shared_ptr<const sandbox_cfg::Config> config_snapshot() noexcept {
    auto snap = g_cfg.current();
    // Never hand back null, so callers can dereference without checking. A
    // default config is the correct answer before anyone has set one: it is
    // exactly the posture the bwrap argv has always built.
    if (!snap) {
        static const auto kDefault =
            std::make_shared<const sandbox_cfg::Config>();
        return kDefault;
    }
    return snap;
}

sandbox_cfg::Config config() {
    // BY VALUE, deliberately, and this is the one place the cost is worth
    // arguing about.
    //
    // It used to return `const Config&` into a plain global. That reference
    // cannot be made safe now that the value is republished under readers:
    // handing out a reference into a snapshot means the snapshot's last
    // owner can drop while the caller still holds the reference, and the
    // caller has no way to know. A reference into shared, swappable state is
    // a dangling pointer with a delay on it.
    //
    // So the UI-facing accessor copies. Callers are form builders and status
    // lines -- a handful per keystroke, three small vectors each. Readers on
    // the hot path (the spawn path, once per command) use config_snapshot()
    // and pay nothing but a refcount.
    return *config_snapshot();
}

bool config_enforceable() noexcept {
    const auto cfg = config_snapshot();
    // Nothing configured ⇒ nothing to ignore. A default config is exactly the
    // posture the bwrap argv already builds, so both backends "enforce" it.
    if (!cfg->configured) return true;
#if defined(__linux__)
    // Off is not a failure to enforce, it is a choice not to sandbox, which
    // the banner already reports on its own.
    if (requested_mode() == Mode::Off) return true;
    return detected_backend() == Backend::Claybin;
#else
    return false;
#endif
}

bool is_active() noexcept {
    return requested_mode() != Mode::Off
        && detected_backend() != Backend::None;
}

std::string describe_state() {
    auto m = requested_mode();
    auto b = detected_backend();
    if (m == Mode::Off) return "sandbox: off";
    const char* tag = nullptr;
    switch (b) {
        case Backend::Claybin:     tag = "claybin";      break;
        case Backend::SandboxExec: tag = "sandbox-exec"; break;
        case Backend::None:        tag = nullptr;        break;
    }
    if (tag) {
        // --workspace / rw-binds the whole filesystem: still wrapped,
        // but no filesystem containment. Be honest about it.
        std::error_code wec;
        auto ws = fs::weakly_canonical(workspace_root(), wec);
        if (wec) ws = workspace_root();
        if (ws == ws.root_path())
            return std::string{"sandbox: degraded ("} + tag
                 + ", --workspace / gives no filesystem containment)";
        return std::string{"sandbox: active ("} + tag + ")";
    }
    if (m == Mode::On)
        return "sandbox: requested but no backend "
#if defined(__linux__)
               "(the kernel offers neither landlock nor seccomp \xe2\x80\x94 "
               "nothing left to build a boundary from; run with --sandbox off)";
#elif defined(__APPLE__)
               "(sandbox-exec missing \xe2\x80\x94 system integrity issue)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if defined(__linux__)
           "(the kernel offers neither landlock nor seccomp)";
#elif defined(__APPLE__)
           "(sandbox-exec missing)";
#else
           "(no backend on this platform)";
#endif
}

// Does this output look like the SANDBOX refused something?
//
// ── Why annotate at all ───────────────────────────────────────────
//
// A bare "Permission denied" is indistinguishable from a real bug, and a model
// reading it concludes the wrong thing with total confidence: it blames a
// corporate proxy, tells the user to check CrowdStrike, or retries the same
// denied call. Every one of those wastes a turn and none can work, because the
// boundary is a kernel decision.
//
// The system prompt says a sandbox exists (see provider/prompt.cpp), and that
// helps -- but it is thousands of tokens away from the error by the time the
// error arrives. Attaching the explanation to the OUTPUT puts it where the
// model is actually looking, at the moment it has to decide what to do next.
// That is the pattern mature sandbox runtimes converge on, and it is the half
// that stops retry loops.
//
// ── Why pattern-matching is sound here ──────────────────────────────
//
// It cannot be exact: the kernel denies, libc stringifies, and the program
// prints whatever it likes. A false POSITIVE costs one extra sentence on an
// error the model was already handling; a false NEGATIVE just leaves today's
// behaviour. Neither can corrupt anything, so matching broadly is the right
// trade -- which is also why this never claims the sandbox DID deny, only that
// it is the likely cause.
[[nodiscard]] bool looks_like_denial(std::string_view out) {
    static constexpr std::string_view kSigns[] = {
        "Permission denied",
        "Operation not permitted",
        "Read-only file system",
        "Network is unreachable",
        "Could not resolve host",
        "Temporary failure in name resolution",
        "Connection refused",
        "Bad system call",            // a seccomp kill, as the shell reports it
    };
    for (auto s : kSigns)
        if (out.find(s) != std::string_view::npos) return true;
    return false;
}

// The note appended to a denied command's output.
//
// Phrased for the MODEL and kept short: it names the cause, forbids the two
// wrong moves (retry, blame the network), and says what to do instead. A note
// that only explained would still leave the model free to retry.
[[nodiscard]] std::string denial_note() {
    const auto cfg = config_snapshot();
    std::string n =
        "\n\n[sandbox] This command ran inside an OS-level sandbox, so a "
        "permission or network error above is most likely the sandbox "
        "refusing it \xe2\x80\x94 not a broken tool, a proxy, or the user's firewall.";
    if (cfg->net_mode == sandbox_cfg::NetMode::None)
        n += " Network access is BLOCKED in this session.";
    else if (cfg->net_mode == sandbox_cfg::NetMode::Ports)
        n += " Only specific ports are reachable in this session.";
    n += " The workspace is writable; most paths outside it are not. "
         "Do not retry the same command unchanged \xe2\x80\x94 work inside the "
         "workspace, or tell the user which wall you hit and what access you "
         "would need.";
    return n;
}

SubprocessResult run_shell_command(std::string_view cmd,
                                   std::size_t max_bytes,
                                   std::chrono::seconds timeout) {
    if (!is_active())
        return run_command_s(std::string{cmd}, max_bytes, timeout);
    auto r = run_wrapped(cmd, max_bytes, timeout);
    // Only on FAILURE, and only when the output looks like a wall. A note on
    // every command would be noise the model learns to skip, which is how an
    // explanation stops working.
    if (r.exit_code != 0 && looks_like_denial(r.output))
        r.output += denial_note();
    return r;
}

SubprocessResult run_argv(const std::vector<std::string>& argv,
                          std::size_t max_bytes,
                          std::chrono::seconds timeout) {
    if (!is_active())
        return run_argv_s(argv, max_bytes, timeout);
    auto r = run_wrapped_argv(argv, max_bytes, timeout);
    // Same annotation as the shell path: this is what hooks and ACP terminals
    // go through, and a hook that dies on a wall is just as easy to
    // misdiagnose as a bash call.
    if (r.exit_code != 0 && looks_like_denial(r.output))
        r.output += denial_note();
    return r;
}

claybin_backend::SpawnResult spawn_in_sandbox(
        const std::vector<std::string>& argv, std::string_view cwd, int out_fd) {
#if defined(__linux__)
    if (detected_backend() == Backend::Claybin) {
        auto posture = build_claybin_posture();
        if (!cwd.empty()) posture.cwd = std::string{cwd};
        return claybin_backend::spawn_argv(posture, argv, out_fd, out_fd);
    }
#else
    (void)argv; (void)cwd; (void)out_fd;
#endif
    claybin_backend::SpawnResult r;
    r.started = false;
    r.start_error = "no claybin backend active";
    return r;
}

claybin_backend::Posture claybin_posture_for_test() {
#if defined(__linux__) || defined(__APPLE__)
    return build_claybin_posture();
#else
    return {};
#endif
}

} // namespace agentty::tools::util::sandbox
