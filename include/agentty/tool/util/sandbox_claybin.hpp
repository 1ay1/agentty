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
};

// One wall and how strongly it is enforced, straight from claybin's guarantee
// report. Exposed so `describe_state()` can name what is actually up instead
// of printing "active".
struct Wall {
    std::string name;
    std::string strength;  // "strong" | "partial" | "none"
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
