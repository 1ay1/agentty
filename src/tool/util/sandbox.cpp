#include "agentty/tool/util/sandbox.hpp"

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/logx.hpp"   // AGT_LOG — the sealed-policy refusal

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>   // shared_ptr — the config snapshot
#include <string>
#include <vector>

// The policy type is a PRODUCT concept, not a claybin one: a build without
// claybin still has a sandbox posture, it just cannot enforce most of it. So
// this include is unconditional and the enforcement is what is guarded --
// otherwise "what is my policy" would be a question only some builds could
// answer, and the settings pane has to work in both.
#include "agentty/domain/sandbox_config.hpp"

// claybin is a required submodule, so the only question left is PLATFORM: it
// compiles a plan anywhere but can only apply one on Linux.
#if defined(__linux__)
#include <unistd.h>  // ::close, for the pidfd the runner does not use

#include "agentty/tool/util/sandbox_claybin.hpp"
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

// Which Linux backend to prefer when both could work. bwrap is the default on
// purpose -- see the note on LinuxPreference in the header: a decade of
// hardening versus days, so the newer one is opt-in rather than a silent
// migration.
std::atomic<LinuxPreference> g_linux_pref{LinuxPreference::Bwrap};

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
// Still a shared_ptr-to-const behind an atomic rather than a plain global:
// written once but READ from tool worker threads and the UI thread at the
// same time, and this is how a reader gets a coherent view without a lock.
//
// Defaults reproduce today's behaviour, so a user who never opens the pane
// gets exactly the sandbox they already had.
std::shared_ptr<const sandbox_cfg::Config> g_cfg{};

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
[[nodiscard]] bool bwrap_can_sandbox() {
    if (!can_invoke("bwrap")) return false;
    const std::vector<std::string> argv = {
        "bwrap",
        "--unshare-user", "--unshare-pid",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind-try", "/bin", "/bin",
        "--ro-bind-try", "/lib", "/lib",
        "--ro-bind-try", "/lib64", "/lib64",
        "--proc", "/proc",
        "--dev", "/dev",
        "--die-with-parent",
        "--", "/bin/true",
    };
    auto r = run_argv_s(argv, /*max_bytes=*/4096, std::chrono::seconds{5});
    return r.started && !r.timed_out && r.exit_code == 0;
}

[[nodiscard]] Backend probe() {
    // claybin only when asked for AND usable. its availability() forks and
    // attempts the uid_map write, the same faithful check bwrap_can_sandbox()
    // does -- so "usable" here means spawn() will work, not that the kernel
    // advertises the feature.
    //
    // note the fallback direction: asking for claybin on a host where it cannot
    // build still yields bwrap, not None. a user who opts into the newer
    // backend should not silently lose their sandbox because of it.
#if defined(__linux__)
    if (g_linux_pref.load(std::memory_order_acquire) == LinuxPreference::Claybin &&
        claybin_backend::available())
        return Backend::Claybin;
#endif
    return bwrap_can_sandbox() ? Backend::Bwrap : Backend::None;
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
    "/.cargo/bin", "/.rustup",   // Rust (bin only from .cargo)
    "/go/bin", "/.go",           // Go (GOPATH bin + webinstall)
    "/.nvm",                     // Node version manager
    "/.pyenv", "/.rbenv", "/.asdf", // version managers
    "/.bun/bin", "/.deno/bin",   // Bun / Deno
    "/.dotnet", "/.sdkman/candidates", // .NET / JVM
};

[[nodiscard]] std::vector<std::string> build_bwrap_argv(std::string_view shell_cmd) {
    std::string ws = workspace_root().string();
    std::vector<std::string> argv = {"bwrap"};

    auto push = [&](const char* a) { argv.emplace_back(a); };
    auto push_pair = [&](const char* k, std::string v) {
        argv.emplace_back(k);
        argv.emplace_back(std::move(v));
    };
    auto push_bind = [&](const char* k, const char* p) {
        argv.emplace_back(k);
        argv.emplace_back(p);
        argv.emplace_back(p);
    };

    // System dirs first: read-only, from the SHARED read set above so the two
    // backends cannot drift. `--ro-bind-try` skips silently if the path doesn't
    // exist on this distro (e.g. /lib64 on Alpine); /usr and /bin are required
    // rather than tried, because a host without them cannot run a shell at all.
    push_bind("--ro-bind",     "/usr");
    push_bind("--ro-bind",     "/bin");
    for (const char* root : kSystemReadRoots) {
        if (std::string_view{root} == "/usr" || std::string_view{root} == "/bin")
            continue;   // already bound as required above
        push_bind("--ro-bind-try", root);
    }

    // /etc: bind ONLY the files a shell + toolchain + name resolution actually
    // need, not the whole tree. See kEtcReadable for why.
    for (const char* f : kEtcReadable) push_bind("--ro-bind-try", f);

    // User-local toolchains. Bound READ-ONLY (execute yes, mutate no) via
    // --ro-bind-try so a missing dir is skipped.
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string h = home;
        for (const char* sub : kHomeToolSubdirs) {
            std::string p = h + sub;
            argv.emplace_back("--ro-bind-try");
            argv.emplace_back(p);
            argv.emplace_back(std::move(p));
        }
    }

    // Pseudo-fs
    push_pair("--proc", "/proc");
    push_pair("--dev",  "/dev");

    // Fresh /tmp inside the sandbox. MUST come before the workspace
    // bind: when workspace lives under /tmp (common in test setups),
    // bwrap applies args in order and a later --tmpfs would wipe the
    // workspace overlay. Bind workspace LAST so it always wins.
    push_pair("--tmpfs", "/tmp");

    // Workspace: read-write. Bound LAST so it overlays any earlier
    // --tmpfs / --ro-bind that touches the same prefix.
    //
    // NOTE: with --workspace / the rw bind covers the entire host
    // filesystem, which defeats the point of the sandbox. We still
    // wrap (process/pid/session hardening + fresh /tmp/proc/dev keep
    // some value) but describe_state() reports the degraded posture so
    // the user isn't told they're "active (bwrap)" when they're not.
    argv.emplace_back("--bind");
    argv.emplace_back(ws);
    argv.emplace_back(ws);

    // Network: keep it. Removing this breaks git push / package
    // installs / curl — flows users explicitly want to work.
    push("--share-net");

    // Process / namespace / privilege hardening. Each --unshare severs a
    // kernel namespace so a command inside the sandbox can't observe or touch
    // the host's view of it:
    //   --unshare-user   own user namespace (uid/gid map) — the root of the
    //                    whole sandbox; requested explicitly so the posture
    //                    matches bwrap_can_sandbox()'s probe.
    //   --unshare-pid    clean PID namespace (kills stay contained; the host
    //                    process table is invisible).
    //   --unshare-ipc    own SysV/POSIX IPC namespace (no shared shm with host
    //                    processes).
    //   --unshare-uts    own hostname/domainname (can't rewrite the host's).
    //   --unshare-cgroup-try  own cgroup view where the kernel allows it.
    //   --new-session    detach the controlling tty so the child can't inject
    //                    into agentty's terminal via TIOCSTI.
    //   --die-with-parent  no detached zombies if agentty exits.
    // bwrap already runs the payload with no ambient capabilities and
    // no_new_privs SET inside the userns, so a setuid binary can't escalate.
    // Network is deliberately KEPT (--share-net) so git/npm/curl work; that is
    // the accepted residual (see the sandboxing doc's threat model).
    push("--unshare-user");
    push("--unshare-pid");
    push("--unshare-ipc");
    push("--unshare-uts");
    push("--unshare-cgroup-try");
    push("--new-session");
    push("--die-with-parent");

    // Pass through the workspace-relative cwd so the shell starts where
    // the user expects. Default cwd would be / inside the sandbox.
    argv.emplace_back("--chdir");
    argv.emplace_back(ws);

    // The actual shell command
    argv.emplace_back("--");
    argv.emplace_back("/bin/sh");
    argv.emplace_back("-c");
    argv.emplace_back(std::string{shell_cmd});
    return argv;
}


// The read set, handed to claybin as DATA rather than restated in its backend.
//
// This function is what keeps the two backends honest. Both consume
// kSystemReadRoots / kEtcReadable / kHomeToolSubdirs, so a change to the
// boundary applies to both or neither. A previous backend restated the set by
// hand and ended up granting read on `/` -- handing ~/.ssh and ~/.aws to any
// approved bash call while still reporting "sandbox: active".
#if defined(__linux__)
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

    // ── secrets, always, plus the user's own masks ────────────────────────
    //
    // kAlwaysMasked is not configurable, and that is the point: a control the
    // user can switch off to make their build work is a control that is off.
    // The pane can ADD masks, never remove these.
    if (const char* home = std::getenv("HOME"); home && *home) {
        for (const char* m : sandbox_cfg::kAlwaysMasked)
            p.masked.emplace_back(std::string{home} + m);
    }
    for (const char* n : sandbox_cfg::kAlwaysMaskedNames) {
        if (n[0] != '.' || std::string_view{n} == ".pem") continue;
        p.masked.emplace_back(p.workspace + "/" + n);
    }
    for (const auto& d : cfg.deny_paths) p.masked.push_back(d);

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
    return p;
}
#endif

[[nodiscard]] SubprocessResult run_wrapped(std::string_view cmd,
                                           std::size_t max_bytes,
                                           std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };

#if defined(__linux__)
    if (detected_backend() == Backend::Claybin) {
        // claybin is a library, so there is no argv prefix to build. It forks,
        // applies the plan, and execs itself -- but only the SPAWN; the
        // supervise loop (poll, progress, idle deadline, SIGTERM/SIGKILL, reap)
        // stays in Subprocess::run, shared with every other path.
        //
        // `command` still carries the shell string so the runner's logging and
        // error messages read the same for both backends.
        opts.command = SubprocessOptions::Shell{std::string{cmd}};
        std::string shell_cmd{cmd};
        opts.spawner = [shell_cmd](const SubprocessOptions&,
                                   int pipe_write_fd) -> SubprocessOptions::SpawnedChild {
            SubprocessOptions::SpawnedChild out;
            auto posture = build_claybin_posture();
            // Both streams onto the one pipe the runner already made, which is
            // what the bwrap path gets from its file_actions.
            auto r = claybin_backend::spawn_shell(posture, shell_cmd, pipe_write_fd,
                                                  pipe_write_fd);
            if (!r.started) {
                out.error = r.start_error;
                return out;
            }
            if (r.pidfd >= 0) ::close(r.pidfd);  // the runner reaps by pid
            out.pid = r.pid;
            return out;
        };
        return Subprocess::run(std::move(opts));
    }
#endif

    opts.command = SubprocessOptions::Argv{build_bwrap_argv(cmd)};
    return Subprocess::run(std::move(opts));
}

// argv-form: wrap with the same bwrap prefix as the shell form, then
// append the user's argv after the `--` separator. No `sh -c`
// indirection — the args reach the child process exactly as given.
[[nodiscard]] SubprocessResult run_wrapped_argv(const std::vector<std::string>& user_argv,
                                                std::size_t max_bytes,
                                                std::chrono::seconds timeout) {
    if (user_argv.empty()) {
        SubprocessResult r;
        r.started = false; r.start_error = "empty argv";
        return r;
    }
    // Build prefix with no shell command, then splice the user's argv.
    auto wrapped = build_bwrap_argv("");
    // Pop the trailing 4 elements added by build_bwrap_argv ("--",
    // "/bin/sh", "-c", ""), then append user argv directly.
    wrapped.resize(wrapped.size() - 4);
    wrapped.emplace_back("--");
    for (const auto& a : user_argv) wrapped.push_back(a);

    SubprocessOptions opts;
    opts.command = SubprocessOptions::Argv{std::move(wrapped)};
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
    return Subprocess::run(std::move(opts));
}

#elif defined(__APPLE__)

[[nodiscard]] Backend probe() {
    return can_invoke("sandbox-exec") ? Backend::SandboxExec : Backend::None;
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
    p += "(allow file-write* (subpath \"/dev/tty\"))\n";
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
    auto profile = build_profile(workspace_root().string());
    opts.command = SubprocessOptions::Argv{{
        "sandbox-exec", "-p", std::move(profile),
        "/bin/sh", "-c", std::string{cmd}
    }};
    opts.max_bytes = max_bytes;
    opts.timeout = timeout;
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
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
    opts.on_progress = [](std::string_view snap) { progress::emit(snap); };
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

LinuxPreference requested_linux_backend() noexcept {
    return g_linux_pref.load(std::memory_order_acquire);
}

void prefer_linux_backend(LinuxPreference p) noexcept {
    // Must be called BEFORE init(), which is what probes. Setting it afterwards
    // would leave g_backend disagreeing with the preference, and every bash
    // call reads g_backend.
    g_linux_pref.store(p, std::memory_order_release);
}

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
    // Still an immutable snapshot behind an atomic pointer rather than a
    // plain global. The seal means it is written once, but it is READ from
    // tool worker threads while the UI thread reads it too, and
    // shared_ptr-to-const is how a reader gets a coherent view without a
    // lock. (It also kept a real use-after-free out of the tree back when
    // this was republishable: Config holds three vectors, and assigning one
    // under a concurrent reader is a freed-buffer walk.)
    auto next = std::make_shared<const sandbox_cfg::Config>(cfg);
    std::atomic_store_explicit(&g_cfg, std::move(next), std::memory_order_release);

    // The policy carries the ENGINE, so sealing it seals the backend too.
    //
    // Only when the user has actually chosen: an unconfigured config holds
    // the struct default (bwrap), and letting that overwrite the preference
    // would make a bare set_config() silently undo --sandbox-backend.
    // main.cpp calls this BEFORE parsing the flag, so the flag still wins.
    if (cfg.configured) {
        prefer_linux_backend(cfg.backend == sandbox_cfg::LinuxBackend::Claybin
                                 ? LinuxPreference::Claybin
                                 : LinuxPreference::Bwrap);
    }
}

bool config_sealed() noexcept {
    return g_cfg_sealed.load(std::memory_order_acquire);
}

void reset_config_for_test() noexcept {
    // Order matters even here: drop the config first, then the seal, so a
    // concurrent reader never sees "unsealed but still holding the old
    // policy" -- which is the one state that would let a second set_config
    // land while a reader is mid-snapshot.
    std::atomic_store_explicit(&g_cfg, std::shared_ptr<const sandbox_cfg::Config>{},
                               std::memory_order_release);
    g_cfg_sealed.store(false, std::memory_order_release);
}

std::shared_ptr<const sandbox_cfg::Config> config_snapshot() noexcept {
    auto snap = std::atomic_load_explicit(&g_cfg, std::memory_order_acquire);
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
        case Backend::Bwrap:       tag = "bwrap";        break;
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
               + std::string{can_invoke("bwrap")
                   ? "(bubblewrap present but unprivileged user namespaces are "
                     "blocked \xe2\x80\x94 e.g. Ubuntu 24.04 AppArmor userns "
                     "restriction or kernel.unprivileged_userns_clone=0; "
                     "allow userns or run with --sandbox off)"
                   : "(install bubblewrap)"};
#elif defined(__APPLE__)
               "(sandbox-exec missing \xe2\x80\x94 system integrity issue)";
#else
               "(unsupported on this platform)";
#endif
    // Mode::Auto + no backend → falling through unsandboxed
    return "sandbox: unavailable, running unsandboxed "
#if defined(__linux__)
           + std::string{can_invoke("bwrap")
               ? "(bubblewrap present but user namespaces are blocked \xe2\x80\x94 "
                 "allow unprivileged userns to enable containment)"
               : "(install bubblewrap to enable)"};
#elif defined(__APPLE__)
           "(sandbox-exec missing)";
#else
           "(no backend on this platform)";
#endif
}

SubprocessResult run_shell_command(std::string_view cmd,
                                   std::size_t max_bytes,
                                   std::chrono::seconds timeout) {
    if (!is_active())
        return run_command_s(std::string{cmd}, max_bytes, timeout);
    return run_wrapped(cmd, max_bytes, timeout);
}

SubprocessResult run_argv(const std::vector<std::string>& argv,
                          std::size_t max_bytes,
                          std::chrono::seconds timeout) {
    if (!is_active())
        return run_argv_s(argv, max_bytes, timeout);
    return run_wrapped_argv(argv, max_bytes, timeout);
}

std::vector<std::string> bwrap_argv_for_test([[maybe_unused]] std::string_view shell_cmd) {
#if defined(__linux__)
    return build_bwrap_argv(shell_cmd);
#else
    return {};
#endif
}

} // namespace agentty::tools::util::sandbox
