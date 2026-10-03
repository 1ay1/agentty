#pragma once
// agentty::tools::util::sandbox::claybin_backend — the library-based Linux
// sandbox backend, alongside the bwrap one.
//
// Header kept free of claybin's own headers so the rest of agentty does not
// need claybin's include path, and so a build with AGENTTY_HAVE_CLAIN off
// compiles this file away entirely.
//
// The interface is deliberately narrow: a Posture (what to bind, what to cap)
// in, a pid out. Everything about SUPERVISING the child -- poll, drain, idle
// deadline, SIGTERM/SIGKILL, reap -- stays in subprocess.cpp, which already
// does it correctly for both the sandboxed and unsandboxed paths. A sandbox
// backend that also owned the supervise loop would be two implementations of
// the hard part.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agentty::tools::util::sandbox::claybin_backend {

// What to build. Populated from the SHARED read-set constants in sandbox.cpp
// rather than restated here -- see the note at the top of sandbox_claybin.cpp
// about a backend that restated them and granted read on `/`.
struct Posture {
    std::vector<std::string> system_read_roots;  // kSystemReadRoots
    std::vector<std::string> etc_readable;       // kEtcReadable
    std::vector<std::string> home_tool_dirs;     // $HOME + kHomeToolSubdirs
    // Extra read/write grants the user added in the pane, on top of the scope.
    std::vector<std::string> read_paths;
    std::vector<std::string> write_paths;
    // Absolute paths masked after every grant. Credentials, agent state, and
    // whatever the user added to deny_paths. See kAlwaysMasked.
    std::vector<std::string> masked;
    // Host-trusted paths that EXIST and must be read-only: .vscode/tasks.json,
    // .git/hooks/*, .git/config, hooks.json. The PREVENTION half of the trust
    // handoff gate.
    //
    // Not masked and not denied -- bound over themselves read-only, so the
    // guest reads the real contents and gets EROFS on write. Masking would
    // break every legitimate reader (git needs its own config); a landlock
    // deny cannot express "this path only" under a writable workspace at all
    // (measured: within one ruleset rights INHERIT and a deeper stricter rule
    // does not subtract -- docs/design/shell-write-gate.md). A read-only bind
    // is exact, needs no grammar, and no quoting trick evades it because the
    // wall is the filesystem.
    //
    // Needs a mount namespace, so this is ABSENT on a host that denies
    // unprivileged userns -- which is why the detection half exists and runs
    // everywhere regardless.
    std::vector<std::string> handoff_ro;
    std::string workspace;                       // read-write, bound last
    std::string cwd;                             // where the shell starts

    // ── network ──────────────────────────────────────────────────────────
    // Mirrors sandbox_cfg::NetMode as an int, so this header stays free of
    // the domain type and a build without claybin does not need it.
    //   0 = Full (share the host netns), 1 = None, 2 = Ports
    int net_mode{0};
    std::vector<std::uint16_t> allow_ports;

    // ── syscalls ─────────────────────────────────────────────────────────
    // sandbox_cfg::SyscallMode as an int: 0 = Off, 1 = Compiler, 2 = Strict.
    int syscall_mode{1};
    bool wx_protect{true};

    // ── hardening ────────────────────────────────────────────────────────
    bool scope_ipc{true};
    bool close_inherited_fds{true};

    // ── resources ────────────────────────────────────────────────
    std::uint64_t tmp_bytes{512ull * 1024 * 1024};
    std::uint64_t memory_bytes{0};   // 0 = no cap
    std::uint64_t max_processes{0};  // 0 = no cap
    std::uint32_t cpu_percent{0};    // 0 = no cap, 100 = one core
    std::uint32_t max_open_files{0}; // 0 = no cap (RLIMIT_NOFILE)
    std::uint32_t wall_clock_secs{0};// 0 = no cap
    std::uint32_t cpu_secs{0};       // 0 = no cap (RLIMIT_CPU)

    // ── identity ────────────────────────────────────────────────
    // Not containment -- the guest cannot escalate either way. This is about
    // what leaks into build output and test fixtures.
    bool fake_hostname{false};

    // ── syscall brokering ────────────────────────────────────────
    // Decide ptrace and kill at runtime from their scalar arguments, instead
    // of denying them flatly. See sandbox_broker.hpp.
    //
    // A FLAG rather than always-on, because brokering makes the child's
    // progress depend on the parent answering: the kernel blocks a brokered
    // syscall until someone responds, so a supervisor that is not being polled
    // is a hang. spawn_shell only sets it when it can hand the listener to a
    // runner that will poll it.
    bool broker{false};
};

// One wall and how strongly it is enforced, straight from claybin's guarantee
// report. Exposed so `describe_state()` can name what is actually up instead
// of printing "active".
struct Wall {
    std::string name;
    std::string strength;  // "strong" | "partial" | "none"
    // WHICH mechanism got it there: "landlock abi 10", "cgroup2 memory.max",
    // "seccomp-bpf", "mount-ns".
    //
    // Not decoration. "strong" alone is a claim; "strong via landlock abi 10"
    // is a claim with its receipt attached, and the difference matters because
    // the SAME capability can be strong by two different routes with different
    // failure modes -- filesystem.read is mount-ns when namespaces work and
    // landlock when they do not, and a user debugging a denial needs to know
    // which. It was missing here while the settings pane already reported it,
    // so the two surfaces disagreed about how much they knew.
    std::string mechanism;
};

struct Report {
    bool ok{false};
    std::string detail;  // why not, when !ok
    std::vector<Wall> walls;
};

struct SpawnResult {
    bool started{false};
    int pid{-1};
    int pidfd{-1};
    std::string start_error;

    // The seccomp listener, when the policy brokers syscalls. -1 otherwise.
    //
    // The CALLER owns it and MUST poll it for as long as the child runs: a
    // brokered syscall blocks the guest thread in the kernel until someone
    // answers, so an unpolled listener is a hang rather than a weaker wall.
    // `service_broker` below is how to answer one.
    int supervisor_fd{-1};

    // Answer ONE pending notification. Returns false when the listener is
    // finished (guest gone, fd closed), after which the caller stops polling.
    //
    // A callback rather than exposing the Listener, so the tool layer never
    // includes a claybin header: the decision policy, the TOCTOU recheck and
    // the audit record all live behind this one call.
    std::function<bool()> service_broker;
};

// Can this host actually build the sandbox? Forks and attempts the uid_map
// write, so it predicts spawn() rather than guessing from sysctls.
[[nodiscard]] bool available();

// Compile the posture and report which walls it yields, without running
// anything.
[[nodiscard]] Report describe(const Posture& p);

// Fork, apply the sandbox, exec `/bin/sh -c shell_cmd`. stdout_fd/stderr_fd are
// the CALLER's descriptors (typically both ends of one pipe); they survive the
// sandbox's close sweep and land on 1 and 2 in the guest. Returns as soon as
// the child is running -- the caller waits.
[[nodiscard]] SpawnResult spawn_shell(const Posture& p, const std::string& shell_cmd,
                                      int stdout_fd, int stderr_fd);

}  // namespace agentty::tools::util::sandbox::claybin_backend
