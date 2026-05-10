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
#include <string>
#include <string_view>
#include <vector>

#include "agentty/tool/util/subprocess.hpp"

namespace agentty::tools::util::sandbox {

enum class Mode : std::uint8_t {
    Off,    // explicitly disabled, never wrap
    Auto,   // wrap if backend available, fall through with warning otherwise
    On,     // wrap if available, fail at startup if backend missing
};

enum class Backend : std::uint8_t {
    None,         // no backend detected / sandbox disabled
    Bwrap,        // Linux bubblewrap
    SandboxExec,  // macOS sandbox-exec
};

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

} // namespace agentty::tools::util::sandbox
