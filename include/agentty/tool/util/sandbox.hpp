#pragma once
// agentty::tools::util::sandbox — OS-native sandbox layer for the bash /
// diagnostics tools.
//
// The workspace boundary in fs_helpers.cpp gates *declared* paths (the
// `path` arg to `read`, `write`, `edit`, etc.). It can't gate what a
// shell command actually does at runtime — `bash "cd /etc && cat passwd"`
// declares no path, runs through `Effect::Exec`, and (under Ask) prompts
// the user once for "this bash call." That's the wrong granularity:
// after one approval the model can do anything inside the OS process
// boundary.
//
// This module wraps the shell command in an OS-native sandbox so even
// an approved bash call is constrained to the workspace + system libs
// + network. Backends:
//
//   Linux   — claybin (vendored library, no binary to install): user
//             namespaces where available, plus landlock, seccomp-bpf and
//             cgroup2. Needs no root and no package.
//   macOS   — sandbox-exec (built in since 10.5). Apple deprecated
//             the public docs but the binary still works on 14+ and
//             takes a Scheme-style profile.
//   Windows — no-op. AppContainer + Job Objects are the right
//             primitives but the surface is large enough that getting
//             it right deserves its own milestone. Documented gap.
//
// The module is opt-in via `--sandbox=on` / opt-out via `--sandbox=off`
// / smart-default `--sandbox=auto` (use if available, warn otherwise).
// An unset CLI flag is auto.
//
// Tools call `run_shell_command(...)` instead of `run_command_s(...)`
// directly — same SubprocessResult shape, but the shell command is
// transparently wrapped when sandbox is active. `is_active()` /
// `describe_state()` are exposed for status surfaces (banner, "agentty
// status", README docs).

#include <chrono>
#include <cstdint>
#include <memory>   // shared_ptr — config_snapshot's return type
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/sandbox_config.hpp"
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace agentty::tools::util::sandbox {

enum class Mode : std::uint8_t {
    Off,    // explicitly disabled, never wrap
    Auto,   // wrap if backend available, fall through with warning otherwise
    On,     // wrap if available, fail at startup if backend missing
};

enum class Backend : std::uint8_t {
    None,         // no backend detected / sandbox disabled
    Claybin,      // Linux, in-process via the claybin library
    SandboxExec,  // macOS sandbox-exec
};

// There is ONE Linux engine, and it is claybin.
//
// bwrap used to be a second, selectable backend. It was removed, and the
// argument for keeping it turned out not to survive the code it described.
// That argument was: the two engines fail on DIFFERENT hosts, so dropping
// bwrap trades a weaker sandbox for no sandbox on kernels without landlock
// (old RHEL). But claybin's availability floor is `landlock_abi > 0 ||
// seccomp` -- either one is enough -- and seccomp-bpf landed in Linux 3.5
// while the unprivileged user namespaces bwrap REQUIRES landed in 3.8. So
// every host that could run bwrap has seccomp, and therefore has claybin.
// The host the argument was defending does not exist.
//
// What claybin gives that the bwrap path never did, measured rather than
// argued (tests/sandbox_audit.cpp):
//
//     syscall.filter         seccomp-bpf        vs bwrap NONE
//     resource.mem/cpu/pids  cgroup2            vs bwrap NONE
//     filesystem.exec        landlock           vs bwrap partial
//     ptrace/kill broker     seccomp-notify     vs bwrap NONE
//
// There was no capability where bwrap won, which is why it was the fallback
// rather than the default, and why nothing is lost by its going.

// Install the user's saved sandbox policy. Call ONCE, at startup, before
// init(): the policy decides what the probe should be checking, so a config
// asking for a wall this kernel cannot build is caught at startup where it
// can be reported, rather than per-command where it looks like a broken tool.
//
// SEALED after the first call -- a later call is ignored and logs a warning.
// The sandbox policy is the one setting in agentty that is NOT live, and that
// is a deliberate trade rather than an implementation limit.
//
// A boundary that can move mid-session only ever moves usefully in the
// WEAKENING direction. The weakening is invisible (loosening a policy
// silently drops a seccomp rule, a cgroup cap, or a secret mask) and it
// cannot be undone, because re-tightening does not un-read a key that
// already left. It also makes the sandbox unauditable: two commands in one
// session get two different walls, with nothing recording which got which.
//
// So the pane writes to disk for the NEXT launch, and restart is the apply
// step. Every other setting in agentty stays live.
void set_config(const sandbox_cfg::Config& cfg);

// Has the policy been sealed (i.e. has set_config already run)? The Sandbox
// pane asks so it can tell the user that edits apply on restart, not now.
[[nodiscard]] bool config_sealed() noexcept;

// Break the seal. TESTS ONLY -- production code has no business calling this.
//
// Worth being precise about what the seal is and isn't, so this hook is not
// mistaken for a hole in it. The seal defends against a specific, real bug:
// the settings reducer calling set_config() on a live process, which is how
// the pane shipped and which silently moved the boundary mid-session. It is
// an invariant enforced in code because the same rule as a comment lasted one
// day.
//
// It is NOT a defence against arbitrary in-process code. Anything that can
// call agentty's own functions has already won -- it could spawn without the
// sandbox entirely. So a clearly-named test hook costs nothing against the
// threat this actually addresses, and the alternative (a fresh process per
// case) would make the sealing behaviour itself untestable.
//
// Named for_test like claybin_posture_for_test below, so a grep for
// `_for_test` finds every seam that exists only for the suite.
void reset_config_for_test() noexcept;

// The policy in force. Read by the settings pane to seed its form, and by
// anything that wants to report the posture.
//
// BY VALUE, not by reference, and not optional: the policy is republished
// (see set_config) while tool threads are reading it, so a reference into
// the live value would be a dangling pointer with a delay on it. Callers are
// form builders and status lines; the copy is three small vectors.
[[nodiscard]] sandbox_cfg::Config config();

// The policy as a shared, immutable snapshot.
//
// This is what anything on a WORKER thread should use, and what the spawn
// path uses: take one snapshot at the top of a command and read the whole
// policy from it. That is cheaper than config() (a refcount, no copy) and it
// is the only way to get a coherent read -- a save landing halfway through
// building a posture would otherwise mix two policies into one sandbox.
//
// Never null; before anyone calls set_config it is the default config, which
// is the posture claybin builds when nobody has tightened anything.
[[nodiscard]] std::shared_ptr<const sandbox_cfg::Config> config_snapshot() noexcept;

// Whether the ACTIVE backend can actually enforce the saved policy.
//
// Only claybin can. The policy's vocabulary -- a syscall profile, per-port
// network rules, W^X, cgroup2 caps, deny-under-grant path masks -- is
// claybin's; macOS sandbox-exec has no spelling for most of it, and Windows
// has no backend at all. So off Linux the pane is configuring something
// inert.
//
// That has to be SAID rather than silently tolerated: a security control the
// user believes they set and that is not running is worse than one they know
// they don't have. main.cpp warns at startup and the pane shows it inline.
// Returns true when there is no policy to enforce (nobody configured one),
// because then there is nothing being ignored.
[[nodiscard]] bool config_enforceable() noexcept;

// Set the requested mode (from --sandbox CLI flag) and probe the
// system for a usable backend. Idempotent; the result is cached.
// Returns false when mode == On and no backend was found — caller
// (main.cpp) should fail loud rather than silently dropping isolation.
[[nodiscard]] bool init(Mode requested);

[[nodiscard]] Mode    requested_mode() noexcept;
[[nodiscard]] Backend detected_backend() noexcept;

// True when sandbox is both requested AND a backend is available —
// i.e. when wrap_shell_command will actually wrap. Used for status
// banners and for skipping wrap on shorter paths (none yet, but the
// flag is the obvious one to query).
[[nodiscard]] bool is_active() noexcept;

// Single-line describe of current state for the startup banner / status
// command. Examples:
//   "sandbox: active (claybin)"
//   "sandbox: off"
//   "sandbox: requested but no backend (kernel has neither landlock nor seccomp)"
[[nodiscard]] std::string describe_state();

// Run a shell command, wrapping it in the active sandbox when one
// exists. Same shape as util::run_command_s — drop-in replacement for
// the bash tool. When sandbox is Off / unavailable, falls through to
// the normal subprocess runner so behavior is preserved.
[[nodiscard]] SubprocessResult run_shell_command(
    std::string_view cmd,
    std::size_t max_bytes,
    std::chrono::seconds timeout);

// argv-form variant for callers that already build a typed argv (e.g.
// `diagnostics` invoking `cmake --build build`). Same wrap policy as
// the shell variant: the sandbox applied when active, no-op otherwise.
// Wraps without going through `sh -c`, preserving exact argv semantics
// that matter for things like commit messages with quotes / `$vars`.
[[nodiscard]] SubprocessResult run_argv(
    const std::vector<std::string>& argv,
    std::size_t max_bytes,
    std::chrono::seconds timeout);

// Escape a path for safe interpolation into the macOS sandbox-exec (SBPL)
// profile string. Exposed for testing the injection-hardening: `\` and `"`
// are backslash-escaped; a path containing a control character (which SBPL
// string literals can't represent) yields an EMPTY string so the caller
// omits the clause and fails closed. A path with no special chars is returned
// unchanged. On non-Apple builds this is a trivial passthrough of the same
// rules (kept cross-platform so the test compiles everywhere).
[[nodiscard]] std::string sbpl_escape(std::string_view path);

// Spawn `argv` inside the active sandbox, with both streams on `out_fd`.
//
// The posture is built HERE rather than handed out, because building it is
// the sandbox's job and a caller that assembles its own would be a second
// policy. exec.cpp used to reach for claybin_posture_for_test() to do this,
// which worked and was a lie: a `_for_test` seam in the production path.
//
// `cwd` is applied as a real directory change by the sandbox, not as a
// `cd ... &&` prefix on a shell string -- the prefix is re-parsed by the
// shell and denied by the boundary, which is how a background session died
// with "cd: Operation not permitted" while the identical foreground command
// worked.
//
// Returns started=false with a reason when no claybin backend is active.
[[nodiscard]] claybin_backend::SpawnResult spawn_in_sandbox(
    const std::vector<std::string>& argv, std::string_view cwd, int out_fd);

// Testing hook: the REAL claybin posture this process would build from the
// sealed config snapshot right now, including the non-configurable masks and
// workspace sweep in sandbox.cpp. Empty/default on non-Linux builds.
[[nodiscard]] claybin_backend::Posture claybin_posture_for_test();

} // namespace agentty::tools::util::sandbox
