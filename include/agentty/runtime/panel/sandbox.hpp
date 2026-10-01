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
inline constexpr std::string_view kSbPosture     = "posture";
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

    // The policy never ASKED for this capability, so a `none` here is a
    // choice rather than a gap.
    //
    // Without this the footer listed `resource.memory: none (none)` on a
    // default policy -- where memory_mb = 0 means "no cap wanted" ever since
    // sandbox_config.hpp decided the right ceiling is a property of the
    // machine. Three such rows sat beside the real degradations and taught the
    // user to read the whole line as noise, which is the opposite of what a
    // wall report is for.
    bool not_requested = false;
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
// One of these two has since been built, and the note is updated rather than
// deleted because the reasoning is still the rule:
//
//   Observation   — a "learning mode" that runs permissively and records what
//                   it would have had to grant, so `npm install` once hands
//                   you the allowlist instead of a sequence of failures.
//                   STILL NOT BUILT.
//   BlockedEvent  — a feed of what the sandbox actually stopped. BUILT: it
//                   lives in tool/util/sandbox_broker.hpp (seccomp-notify
//                   denials) with a sibling in tool/util/handoff_gate.hpp for
//                   trust handoffs, and the pane footer renders both. It is
//                   not declared here because the producer owns the type —
//                   the pane reads the feed, it does not define it.
//
// The rule that kept the structs out of this header until something populated
// them: a type with a reducer arm and no producer is the same trap kSbMode
// was. It reads as a shipped feature to the next person, and it makes the pane
// look like it reports things it cannot. See docs/design/sandbox-boundary.md
// §13 for the learning mode's implementation notes and §14 for the feeds.
//
// The learning mode carries a warning if it ever lands: it is a WEAKER sandbox
// while it runs, and a mode that silently weakens a boundary is worse than no
// mode. It has to be loud, and it cannot be the default.

// What the host can do, and what is ACTUALLY enforcing right now.
//
// ── Why this is a struct and not three parameters ────────────────────
//
// Because conflating two of these shipped the worst bug this pane has had.
// `build_sandbox_form` used to take `claybin_available` and nothing else, so
// the only facts in scope were the SELECTED engine and whether the host COULD
// run it. The subtitle was built from those two -- and therefore announced
// "claybin \xc2\xb7 landlock abi 10 \xc2\xb7 seccomp \xc2\xb7 cgroup2" the instant you moved the
// row, on a process that was still running bwrap and could not change.
//
// That is issue #21 exactly, re-entered through the front door: a pane built
// to stop "sandbox: active" from meaning nothing, claiming walls that were not
// up. The missing fact was never computed anywhere the view could see it --
// `SandboxPane::backend` held it and had no reader at all.
//
// So the running engine is a NAMED FIELD next to the capability probe, and
// every caller has to supply it. There is no overload that omits it.
struct HostFacts {
    // Can claybin start here? A real probe (it forks and attempts the uid_map
    // write), not a guess.
    bool claybin_available = false;
    std::uint32_t landlock_abi = 0;

    // What is enforcing RIGHT NOW. Sealed at startup and immutable for the
    // life of the process -- which is the whole reason it has to be separate
    // from the selection: the row can change, this cannot.
    sandbox_cfg::LinuxBackend running = sandbox_cfg::LinuxBackend::Bwrap;

    // False when there is no backend at all (probe failed, or --sandbox off).
    // `running` is meaningless then, and the subtitle must say so rather than
    // naming an engine that is not confining anything.
    bool sandbox_active = false;
};

// The pane. Named SandboxPane, not Sandbox, because panel/slot.hpp needs the
// bare name for the SLOT that holds it -- same split as AppearancePane.
struct SandboxPane {
    form::Form form;

    // Recomputed on every edit, so the walls track the rows.
    Preview preview;

    // Whether the NEXT LAUNCH will actually deliver what the rows describe,
    // and if not, why. Empty means the promise is good.
    //
    // Computed by the reducer alongside the preview, for the same reason
    // `facts` is stored: the view cannot probe, and it must not re-derive the
    // config to find out. See restart_outcome().
    std::string restart_note;

    // Which backend is actually in use, for the header line. A pane that
    // offers per-port network while running under bwrap would be lying, so
    // the rows that need claybin are marked unavailable rather than hidden --
    // hiding them would make the limitation invisible.
    std::string backend;        // "claybin" | "bwrap" | "none"
    bool claybin_available = false;

    // The host as measured, captured by the REDUCER at open and on every
    // reproject.
    //
    // Stored rather than probed on demand because the view must stay pure --
    // it cannot fork a uid_map probe mid-render. And it has to be here rather
    // than reconstructed in the view from `backend`/`claybin_available`,
    // because reconstructing it is exactly how the subtitle came to describe
    // a boundary that was not up (§17): the view would be inferring reality
    // from fields that only approximate it.
    HostFacts facts;

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

// The three states the engine row can be in.
//
// A sum type rather than the two bools that caused the bug, for the reason
// panel/form.hpp gives about FormFocus: two bools encoding three states leaves
// the invariant living only in a comment, and this file has now shipped that
// failure twice (kSbMode, and the subtitle). Spelling the states out means the
// view cannot accidentally describe a boundary that is not up.
enum class EngineStatus : std::uint8_t {
    // The selected engine IS the running one. Only in this state may anything
    // on screen speak in the present tense.
    InForce,
    // The selection differs from what is running. A save takes effect at the
    // next launch; the walls described below are a FORECAST.
    AppliesOnRestart,
    // claybin is selected and the host refused it. Saving is still legitimate
    // (the user may be on a different machine tomorrow) but nothing here will
    // be enforced by it.
    CannotStart,
};

[[nodiscard]] EngineStatus engine_status(const sandbox_cfg::Config& cfg,
                                         const HostFacts& facts);

// Will this config actually work on the next launch?
//
// ── Why this is its own function ───────────────────────────────────
//
// Because the pane could say "saved \xc2\xb7 applies on restart" and be wrong about
// it. Every ingredient of the answer was already computed -- the compile
// result, the unenforceable list, the host probe -- and nothing joined them
// into the one claim the footer was making. A promise about the future with no
// check behind it is the same shape as every other bug in this subsystem:
// "sandbox: active" (§1), the handoff row that enforced nothing (§14), the
// subtitle naming walls that were not up (§17). Here the lie just points
// forward instead of at the present.
//
// Three things can make a restart NOT deliver what the rows describe:
//   1. the policy does not compile at all (claybin refuses rather than
//      degrading, so this is a hard no)
//   2. the selected engine cannot start on this host
//   3. it compiles and starts, but some capability silently comes out weaker
//      than asked -- the `unenforceable` list
//
// Returns empty when the restart really will deliver the policy as described.
// A non-empty string is what the footer must say INSTEAD of a bare promise.
[[nodiscard]] std::string restart_outcome(const sandbox_cfg::Config& cfg,
                                          const HostFacts& facts,
                                          const Preview& preview);

// One line describing what is ACTUALLY confining commands right now, derived
// only from `facts` -- it cannot see the selection, so it cannot be talked
// into describing a policy that is not running.
[[nodiscard]] std::string describe_running(const HostFacts& facts);

// Build the form from a config. Pure: no I/O, no probe -- the caller supplies
// what the host can do, so this is testable without a kernel.
[[nodiscard]] form::Form build_sandbox_form(const sandbox_cfg::Config& cfg,
                                            const HostFacts& facts);

// Read the form back into a config. The inverse of the above, and the only
// place that knows the mapping -- so a renamed row breaks in one spot.
[[nodiscard]] sandbox_cfg::Config read_sandbox_form(const form::Form& f,
                                                    const sandbox_cfg::Config& base);

// Compile the config and describe the resulting walls, without spawning.
[[nodiscard]] Preview preview_sandbox(const sandbox_cfg::Config& cfg);

// Attach the wall report to the ROWS, and validate what the user typed.
//
// ── Why this is a separate pass ─────────────────────────────────────────
//
// Because the honesty only exists after the compile. build_sandbox_form()
// knows the config and the host, which is enough to LOCK a row ("this kernel
// cannot do per-port network") but not enough to annotate one ("your network
// rule is enforced by landlock abi 10"). That second fact comes out of
// claybin's GuaranteeReport, which needs the finished config. So: build, then
// compile, then annotate.
//
// ── Why per-row and not just the footer ────────────────────────────────
//
// The footer already carried all of this, and that was the problem. Twelve
// capabilities folded into one run-on line is a paragraph, and a paragraph is
// what you skip. The user's actual question is never "what are all the walls",
// it is "is the thing I just changed real" -- which is a question about ONE
// row. Answering it next to that row turns a wall of text into a glance, and
// it costs nothing: `Field::origin` already renders dim and right-aligned, for
// exactly this ("where did this value come from") on the other panes.
//
// Validation lands in the same pass for the same reason. `Field::error` has
// been in the form model since the beginning and no pane set it; meanwhile
// this pane could accept `net_mode = ports` with an empty port list, which
// compiles to an isolated namespace that denies everything -- the user asked
// for "these ports" and silently got "nothing". A row that is self-defeating
// should say so where it is typed.
void annotate_sandbox_form(form::Form& f, const Preview& preview,
                           const sandbox_cfg::Config& cfg, EngineStatus status);

}  // namespace agentty::ui::panel
