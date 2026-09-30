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
//   Linux   — bwrap (bubblewrap). Common: `apt install bubblewrap`,
//             `dnf install bubblewrap`, `pacman -S bubblewrap`. Used
//             by Flatpak; well-maintained; doesn't need root.
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
#include "agentty/tool/util/subprocess.hpp"

namespace agentty::tools::util::sandbox {

enum class Mode : std::uint8_t {
    Off,    // explicitly disabled, never wrap
    Auto,   // wrap if backend available, fall through with warning otherwise
    On,     // wrap if available, fail at startup if backend missing
};

enum class Backend : std::uint8_t {
    None,         // no backend detected / sandbox disabled
    Bwrap,        // Linux bubblewrap, exec'd as a binary
    Claybin,      // Linux, in-process via the claybin library
    SandboxExec,  // macOS sandbox-exec
};

// Which Linux engine applies the policy. Both are always compiled in --
// claybin is a required submodule, not a build flag -- so this is purely a
// runtime choice, settable with --sandbox-backend or in the Sandbox pane.
//
// bwrap stays the DEFAULT: it is what users already have and has a decade of
// upstream hardening, so an upgrade changes nobody's boundary. claybin adds
// the walls bwrap cannot express (a syscall filter, landlock, cgroup2 caps),
// which is why most of the pane's rows only apply under it.
//
// Asking for claybin on a host where it cannot build still yields bwrap, not
// None: opting into the newer backend must not cost you your sandbox.
enum class LinuxPreference : std::uint8_t {
    Bwrap,    // default
    Claybin,  // --sandbox-backend=claybin, or the pane's Backend row
};

// Which Linux backend the user ASKED for. Distinct from
// detected_backend(), which is what the probe could actually deliver: asking
// for claybin on a host without user namespaces still yields bwrap, and the
// difference between "asked" and "got" is exactly what the Sandbox pane
// needs to explain a locked row.
[[nodiscard]] LinuxPreference requested_linux_backend() noexcept;

// Choose the Linux backend. MUST be called before init(), which is what
// probes -- setting it later would leave the cached backend disagreeing with
// the preference.
void prefer_linux_backend(LinuxPreference p) noexcept;

// Install the user's saved sandbox policy. Also before init(): the policy
// decides what the probe should be checking, so a config asking for a wall
// this kernel cannot build is caught at startup where it can be reported,
// rather than per-command where it looks like a broken tool.
//
// Never called ⇒ the shipped defaults, which reproduce the pre-config
// behaviour exactly.
void set_config(const sandbox_cfg::Config& cfg);

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
// is exactly the posture the bwrap path has always built.
[[nodiscard]] std::shared_ptr<const sandbox_cfg::Config> config_snapshot() noexcept;

// Whether the ACTIVE backend can actually enforce the saved policy.
//
// Only claybin can. The policy's vocabulary -- a syscall profile, per-port
// network rules, W^X, cgroup2 caps, deny-under-grant path masks -- is
// claybin's; bwrap as we invoke it has no spelling for most of it, and the
// parts it could express are fixed at the argv we build. So under bwrap the
// pane is configuring something inert.
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
//   "sandbox: active (bwrap)"
//   "sandbox: off"
//   "sandbox: requested but no backend (install bubblewrap)"
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
// the shell variant: bwrap / sandbox-exec prepended when active, no-op
// otherwise. Wraps without going through `sh -c`, preserving exact
// argv semantics that matter for things like commit messages with
// quotes / `$vars`.
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

// Testing hook: the bwrap argv this build would wrap `shell_cmd` in, WITHOUT
// running it (so tests can assert the hardening flags + read-only toolchain
// binds + workspace RW bind are present on any host, even one without bwrap or
// user namespaces). Empty on non-Linux builds. Exposed for the unit test only;
// production code calls run_shell_command / run_argv.
[[nodiscard]] std::vector<std::string> bwrap_argv_for_test(std::string_view shell_cmd);

} // namespace agentty::tools::util::sandbox
