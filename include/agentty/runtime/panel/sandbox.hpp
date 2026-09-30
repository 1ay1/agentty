#pragma once
// agentty::ui::panel::Sandbox — the sandbox pane.
//
// A form pane like Appearance and Retrieval: the pane holds a form::Form and
// the row ids, so navigation/dropdowns/editing stay the shared reducer's job.
// What is genuinely specific lives here.
//
// ── Why this pane is different from every other agent's sandbox config ───
//
// Everyone's sandbox settings are a list of switches you set once and never
// see again, and that is the actual problem. A boundary you cannot observe is
// a boundary you cannot trust: the failure mode is not "the switch was wrong",
// it is "the switch said active and nothing was enforced". agentty has shipped
// exactly that bug (issue #21: "sandbox: active" while every command died on
// a uid_map denial), and so had claybin's own probe until it was fixed.
//
// So this pane does three things no other agent's does, and they are the
// reason it exists rather than a settings row:
//
//   1. IT SHOWS THE WALLS, LIVE. claybin compiles a policy into a plan and
//      reports, per capability, whether enforcement is `strong`, `partial` or
//      `none` -- and WHICH mechanism got it there (landlock abi 6, cgroup2
//      memory.max, seccomp-bpf). That report is proof-carrying: a backend
//      cannot claim a guarantee it did not install. The pane renders it, so
//      changing a row shows you the wall move. Nobody else can do this,
//      because a sandbox you shell out to has no report to give.
//
//   2. IT SHOWS WHAT WAS BLOCKED. seccomp-notify lets the supervisor see a
//      denied syscall as it happens. So the pane can say "3 blocked: ptrace,
//      clone(CLONE_NEWUSER), /etc/shadow" instead of leaving you to guess why
//      a build failed. A sandbox that fails silently trains people to turn it
//      off; one that narrates what it stopped teaches them what their tools
//      actually do.
//
//   3. IT CAN LEARN A POLICY. Run your workload under Observe, and the
//      sandbox records what it genuinely needed -- which paths, which ports,
//      which syscalls -- then offers the MINIMAL policy that still works.
//      This is the answer to the real reason people run agents unsandboxed:
//      not that they want the risk, but that writing a correct allowlist by
//      hand is miserable and breaks their build three times first.
//
// ── Everything here is claybin ───────────────────────────────────────────
// Per-port network, syscall filtering, cgroup2 caps and the guarantee report
// are all things you can only ask for if you are COMPILING a policy. bwrap
// stays available as a backend, but it cannot answer these questions, so the
// pane is honest about which rows it can enforce and says so per row.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/sandbox_config.hpp"
#include "agentty/runtime/panel/form.hpp"

namespace agentty::ui::panel {

// Row ids. Named constants at both the build and read site, for the reason
// appearance.hpp gives: a typo across two matching string literals is a
// silently dead setting and the compiler cannot see it.
//
// There is deliberately NO `mode` row. Turning the sandbox off is a launch
// decision (`--sandbox off`), not a setting: a saved "off" is a foot-gun that
// survives reboots silently, and the pane cannot change the live boundary
// anyway (the policy is sealed at startup -- see tool/util/sandbox.hpp). A row
// id for a row that must not exist is an invitation to wire it up "for
// consistency", so it is not here to be found. See docs/design/
// sandbox-boundary.md §3.
inline constexpr std::string_view kSbBackend     = "backend";
inline constexpr std::string_view kSbFsScope     = "fs_scope";
inline constexpr std::string_view kSbReadPaths   = "read_paths";
inline constexpr std::string_view kSbWritePaths  = "write_paths";
inline constexpr std::string_view kSbDenyPaths   = "deny_paths";
inline constexpr std::string_view kSbNetMode     = "net_mode";
inline constexpr std::string_view kSbPorts       = "allow_ports";
inline constexpr std::string_view kSbSyscalls    = "syscall_mode";
inline constexpr std::string_view kSbWxProtect   = "wx_protect";
inline constexpr std::string_view kSbMemoryMb    = "memory_mb";
inline constexpr std::string_view kSbMaxProcs    = "max_procs";
inline constexpr std::string_view kSbCpuPercent  = "cpu_percent";
inline constexpr std::string_view kSbOpenFiles   = "max_open_files";
inline constexpr std::string_view kSbWallClock   = "wall_clock_secs";
inline constexpr std::string_view kSbCpuSecs     = "cpu_secs";
inline constexpr std::string_view kSbTmpMb       = "tmp_mb";
inline constexpr std::string_view kSbScopeIpc    = "scope_ipc";
inline constexpr std::string_view kSbCloseFds    = "close_fds";
inline constexpr std::string_view kSbFakeHost    = "fake_hostname";
inline constexpr std::string_view kSbMaskDepth   = "mask_scan_depth";
// The row that defends the escape class nobody else defends. See
// domain/sandbox_provenance.hpp for why it is its own control.
inline constexpr std::string_view kSbHandoff     = "handoff";

// ── The live wall report ────────────────────────────────────────────────
//
// One capability and what is actually enforcing it, straight from claybin's
// GuaranteeReport. `mechanism` is the load-bearing field: "strong" alone is a
// claim, "strong via landlock abi 10" is a claim with its receipt attached.
struct Wall {
    std::string name;       // "filesystem.write", "network.isolation", …
    std::string strength;   // "strong" | "partial" | "none"
    std::string mechanism;  // "landlock abi 10", "cgroup2 memory.max", …
};

// What the pane knows about the current policy, recomputed whenever a row
// changes. This is a COMPILE, not a spawn: claybin's compile() is pure over a
// described host, so the pane can show the consequences of an edit without
// running anything.
struct Preview {
    bool                compiled = false;
    std::string         error;        // why not, when !compiled
    std::vector<Wall>   walls;
    std::size_t         plan_ops = 0; // how many operations the sandbox performs
    // Capabilities the policy ASKED for that this host cannot enforce. Never
    // silently empty -- if a user asks for per-port network on a kernel with
    // landlock abi 3, the pane says so rather than quietly downgrading.
    std::vector<std::string> unenforceable;
};

// ── Deliberately not here yet ─────────────────────────────────────────────
//
// Two features were designed here before they existed, and their types sat in
// this header for a while with nothing populating them:
//
//   Observation   — a "learning mode" that runs permissively and records what
//                   it would have had to grant, so `npm install` once hands
//                   you the allowlist instead of a sequence of failures.
//   BlockedEvent  — a feed of what the sandbox actually stopped, from
//                   seccomp-notify and landlock denials. "cargo tried
//                   ptrace(PTRACE_ATTACH) and was denied" teaches what your
//                   toolchain does; "your build failed" teaches nothing, and
//                   is the difference between adding one allowlist line and
//                   turning the sandbox off.
//
// Both are worth building. Neither is built, so the structs are gone: a type
// with a reducer arm and no producer is the same trap kSbMode was -- it reads
// as a shipped feature to the next person, and it makes the pane look like it
// reports things it cannot. See docs/design/sandbox-boundary.md §13 for the
// implementation notes.
//
// The learning mode carries a warning if it ever lands: it is a WEAKER sandbox
// while it runs, and a mode that silently weakens a boundary is worse than no
// mode. It has to be loud, and it cannot be the default.

// The pane. Named SandboxPane, not Sandbox, because panel/slot.hpp needs the
// bare name for the SLOT that holds it -- same split as AppearancePane.
struct SandboxPane {
    form::Form form;

    // Recomputed on every edit, so the walls track the rows.
    Preview preview;

    // Which backend is actually in use, for the header line. A pane that
    // offers per-port network while running under bwrap would be lying, so
    // the rows that need claybin are marked unavailable rather than hidden --
    // hiding them would make the limitation invisible.
    std::string backend;        // "claybin" | "bwrap" | "none"
    bool claybin_available = false;

    // A save has landed on disk but the LIVE boundary is still the one this
    // process started with. Drives the "applies on restart" footer.
    //
    // Needed because this pane's save is the one place in agentty where
    // saving does not take effect: the sandbox policy is sealed at startup
    // (see tool/util/sandbox.hpp's set_config). Without saying so, a user
    // who tightens the syscall profile and keeps working would believe in a
    // wall that is not up until they relaunch -- which is the exact class of
    // lie this pane exists to prevent, just pointed at the future instead of
    // the present.
    bool saved_pending_restart = false;
};

// (visual_parts for this pane lives in panel/visual_parts.hpp, alongside
// every other panel's -- that header is where the gate's proofs are kept.)

// Build the form from a config. Pure: no I/O, no probe -- the caller supplies
// what the host can do, so this is testable without a kernel.
[[nodiscard]] form::Form build_sandbox_form(const sandbox_cfg::Config& cfg,
                                            bool claybin_available,
                                            std::uint32_t landlock_abi);

// Read the form back into a config. The inverse of the above, and the only
// place that knows the mapping -- so a renamed row breaks in one spot.
[[nodiscard]] sandbox_cfg::Config read_sandbox_form(const form::Form& f,
                                                    const sandbox_cfg::Config& base);

// Compile the config and describe the resulting walls, without spawning.
[[nodiscard]] Preview preview_sandbox(const sandbox_cfg::Config& cfg);

}  // namespace agentty::ui::panel
