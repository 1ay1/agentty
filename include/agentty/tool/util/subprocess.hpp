#pragma once
// Cross-platform subprocess runner.
//
// Why a class-with-Options instead of free functions: we already had three
// overlapping runners (shell / argv / legacy-string-wrapper) each with their
// own truncation / timeout / progress logic. Consolidating into one entry
// point lets every tool pick the knobs it cares about without duplicating
// 200 lines of Win32 pipe plumbing per call site.
//
// Platform specifics:
//   Windows → CreateProcessW with stdin redirected to NUL (prevents the
//             child from stealing keystrokes) and stdin's console mode saved
//             + restored (prevents a child resetting ENABLE_LINE_INPUT from
//             corrupting TUI input). Reader thread drains the pipe so a
//             grandchild that inherits stdout can't deadlock the wait.
//   POSIX   → posix_spawn + poll-based deadline. Shell form goes through
//             /bin/sh -c; argv form execs directly. Timeouts are enforced
//             in-process via SIGTERM (with a 2 s grace) → SIGKILL — no
//             dependency on GNU coreutils `timeout`, which isn't on stock
//             macOS. stdin redirected from /dev/null; stdout+stderr both
//             dup2'd onto a single pipe so callers see merged output.
//
// Both paths stream captured bytes through the thread-local progress sink
// (see agentty/tool/registry.hpp) at most every ~80 ms, so the UI reveals live
// output without flooding the event queue.

#include <chrono>
#include <functional>
#include <optional>
#include <variant>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::tools::util {

struct SubprocessOptions {
    // WHAT to run — exactly one form, chosen by the type.
    //
    //   Argv{...}  exec'd directly, no shell: paths, refs, commit
    //              messages and format strings survive intact.
    //   Shell{...} goes through cmd.exe / sh and gets its quoting rules.
    //
    // This was two independent optionals with a comment saying "exactly
    // one of shell_command / argv must be set". Two optionals describe
    // FOUR states — both set and neither set are equally constructible —
    // and nothing enforced the rule: the runner is an `if (shell) … else
    // if (argv) …` chain, so "both" silently ran the shell form and
    // "neither" fell out the bottom. For an API that spawns processes,
    // the difference between shell and exec is a security boundary, and
    // it should not be expressible ambiguously.
    struct Argv  { std::vector<std::string> args; };
    struct Shell { std::string command; };
    using Command = std::variant<Argv, Shell>;
    Command command{Argv{}};

    // Convenience readers, so call sites that only care about one form
    // don't unpack the variant by hand.
    [[nodiscard]] const std::vector<std::string>* argv_if() const noexcept {
        if (auto* a = std::get_if<Argv>(&command)) return &a->args;
        return nullptr;
    }
    [[nodiscard]] const std::string* shell_if() const noexcept {
        if (auto* s = std::get_if<Shell>(&command)) return &s->command;
        return nullptr;
    }

    std::chrono::seconds timeout{120};
    // Unsigned because a negative cap makes no sense and every compare
    // site was already a `size_t` on the RHS; the old `int` caused
    // mixed-sign promotions and the occasional sign-compare warning.
    std::size_t          max_bytes = 30'000;

    // Called with the full accumulated buffer (not a delta) on a best-effort
    // throttle. Passing the whole buffer each time means multi-byte UTF-8
    // sequences that span pipe reads still render correctly on the next
    // flush — no need for delta-aware splitting on the caller side.
    std::function<void(std::string_view snapshot)> on_progress;

    // Optional cooperative stop probe.  The runner checks it at most once per
    // supervise-loop iteration and terminates the child process when true.
    // This keeps callers such as a provider bridge responsive to UI cancel
    // without coupling this low-level utility to a particular token type.
    std::function<bool()> stop_requested;
};

struct SubprocessResult {
    std::string output;                // captured stdout+stderr, UTF-8 valid
    int  exit_code   = 0;
    bool timed_out   = false;
    bool truncated   = false;
    bool started     = true;           // false iff spawn itself failed
    std::string start_error;           // populated when started==false
};

struct Subprocess {
    [[nodiscard]] static SubprocessResult run(SubprocessOptions opts);
};

// ── Convenience wrappers around Subprocess::run ─────────────────────────
//
// `run_command_s` takes a shell string, `run_argv_s` takes a pre-built argv
// (no shell). The `_s` suffix is a throwback to the pre-refactor era where
// the non-`_s` versions returned the "legacy_format" suffixed string shape;
// kept for call-site grep-ability.

[[nodiscard]] SubprocessResult run_command_s(
    const std::string& cmd,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

[[nodiscard]] SubprocessResult run_argv_s(
    const std::vector<std::string>& argv,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

// Flatten a SubprocessResult into the legacy suffix-marker string shape:
//   <output>[\n[output truncated]][\n[timed out after Xs] | \n[exit code N]]
// Tools that parse exit codes out of their captured blob (e.g. git_commit's
// `out.find("[exit code")` guard) depend on this format — don't change it
// without auditing every caller.
[[nodiscard]] std::string legacy_format(const SubprocessResult& r,
                                        std::chrono::seconds timeout);

[[nodiscard]] std::string run_command(
    const std::string& cmd,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

[[nodiscard]] std::string run_argv(
    const std::vector<std::string>& argv,
    std::size_t          max_bytes = 30'000,
    std::chrono::seconds timeout   = std::chrono::seconds{120});

} // namespace agentty::tools::util
