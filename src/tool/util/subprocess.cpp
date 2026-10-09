#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/io/fsm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentty/tool/util/exec.hpp"   // run_child: the one supervise loop

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace agentty::tools::util {

namespace {

// Terminal line-discipline + UTF-8 repair, applied to EVERY byte that
// leaves the subprocess runners — live progress snapshots and the final
// captured output alike. The live path is the one that used to leak raw
// escapes: bash progress snapshots went straight to the UI card, so a
// child that thought it owned a tty (ls --color, cargo, top -b, anything
// probing with DECSTBM/SGR) painted its CSI parameter bytes as literal
// glyphs ("[1;24r" → stray "r" cells), which then committed to native
// scrollback — the reported per-frame corruption during bash tool use.
std::string clean_capture(std::string s) {
    return to_valid_utf8(strip_terminal_controls(s));
}

} // namespace

namespace {

#ifdef _WIN32

// UTF-8 → UTF-16. Win32 process APIs are UTF-16 natively; anything narrower
// routes through the ANSI code page and silently corrupts non-ASCII bytes.
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

// CommandLineToArgvW-compatible quoting (see MSDN "Parsing C++ Command-Line
// Arguments"). Run of backslashes doubles if followed by `"`, literal `"`
// gets prefixed with `\`. Quoting only wraps the arg when it contains
// whitespace or `"`.
std::string win_quote_arg(const std::string& arg) {
    if (!arg.empty()
        && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.push_back('"');
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { backslashes++; continue; }
        if (c == '"') {
            out.append((size_t)backslashes * 2, '\\');
            out += "\\\"";
        } else {
            out.append((size_t)backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append((size_t)backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

// Return true when the argv path resolves to a .bat / .cmd file on PATH.
// CreateProcess cannot natively spawn batch files — they require cmd.exe —
// but tools like npx / npm / yarn ship as .cmd on Windows, and the model
// will reach for them. SearchPathW honors PATHEXT so .cmd is found even
// when the caller wrote just "npx".
bool resolves_to_batch(const std::string& exe) {
    auto we = utf8_to_wide(exe);
    wchar_t out[MAX_PATH * 2]{};
    DWORD n = ::SearchPathW(nullptr, we.c_str(), L".exe",
                            (DWORD)std::size(out), out, nullptr);
    if (n == 0 || n >= std::size(out)) {
        // .exe miss — try default PATHEXT order (batch files second).
        n = ::SearchPathW(nullptr, we.c_str(), nullptr,
                          (DWORD)std::size(out), out, nullptr);
        if (n == 0 || n >= std::size(out)) return false;
    }
    std::wstring_view resolved{out, n};
    auto dot = resolved.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    auto ext = resolved.substr(dot);
    auto ieq = [](wchar_t a, wchar_t b) {
        return (a >= L'A' && a <= L'Z' ? a - L'A' + L'a' : a)
            == (b >= L'A' && b <= L'Z' ? b - L'A' + L'a' : b);
    };
    auto equal_i = [&](std::wstring_view s, std::wstring_view t) {
        if (s.size() != t.size()) return false;
        for (size_t i = 0; i < s.size(); ++i) if (!ieq(s[i], t[i])) return false;
        return true;
    };
    return equal_i(ext, L".cmd") || equal_i(ext, L".bat");
}

#endif

// One supervise loop for every child agentty runs: run_child (exec.cpp) on
// jaal's native process. This only maps the options across. `win_cmdline`,
// when set, is the verbatim Windows command line.
SubprocessResult run_via_child(std::vector<std::string> argv, const SubprocessOptions& opts,
                               std::string win_cmdline = {}) {
    ChildRun run;
    run.argv = std::move(argv);
    run.windows_command_line = std::move(win_cmdline);
    run.idle = opts.timeout;
    run.wall = [&]() -> std::chrono::seconds {
        // $AGENTTY_TOOL_HARD_TIMEOUT_SECS overrides for the whole process;
        // 0 switches the ceiling off.
        if (const char* e = std::getenv("AGENTTY_TOOL_HARD_TIMEOUT_SECS"); e && e[0]) {
            char* end = nullptr;
            const long v = std::strtol(e, &end, 10);
            if (end != e && v >= 0) return std::chrono::seconds{v};
        }
        if (opts.hard_timeout.count() > 0) return opts.hard_timeout;
        if (opts.timeout.count() <= 0)     return std::chrono::seconds::zero();
        return std::max<std::chrono::seconds>(opts.timeout * 20, std::chrono::minutes{10});
    }();
    run.max_output_bytes = opts.max_bytes;
    run.stop_requested   = opts.stop_requested;
    if (opts.on_progress)
        run.on_progress = [&opts](std::string_view raw) { opts.on_progress(clean_capture(std::string{raw})); };
#if !defined(_WIN32)
    if (opts.spawner) {
        run.spawn_adopted = [&opts](int out_fd) {
            auto c = opts.spawner(opts, out_fd);
            AdoptedChild a;
            a.pid   = c.pid;
            a.pidfd = c.pidfd;
            if (c.pid < 0) a.error = c.error.empty() ? "sandbox spawner failed" : c.error;
            a.supervisor_fd  = c.supervisor_fd;
            a.service_broker = std::move(c.service);
            a.kill_tree      = std::move(c.kill_tree);
            return a;
        };
    }
#endif

    auto c = run_child(run);
    SubprocessResult r;
    r.started     = c.started;
    r.start_error = c.start_error;
    r.output      = clean_capture(std::move(c.output));
    r.truncated   = c.truncated;
    r.timed_out   = c.timed_out_idle || c.timed_out_wall;
    r.hard_capped = c.timed_out_wall;
    r.exit_code   = c.exited ? c.exit_code : 128 + c.signal;
    return r;
}

} // namespace

SubprocessResult Subprocess::run(SubprocessOptions opts) {
    SubprocessResult r;

    // Build a final command line appropriate for this platform.
#ifdef _WIN32
    std::string cmdline;
    if (const std::string* sh = opts.shell_if()) {
        // cmd.exe /S /C "…" — /S strips just the outermost quotes and
        // leaves everything else (including embedded "...") intact.
        cmdline = "cmd.exe /S /C \"" + *sh + "\"";
    } else {
        const std::vector<std::string>& av = *opts.argv_if();
        if (av.empty()) {
            r.started = false; r.start_error = "empty command"; return r;
        }
        // If argv[0] is a .cmd / .bat on PATH, wrap through cmd.exe — raw
        // CreateProcess refuses batch files (they need the interpreter).
        // Checked once up-front so we don't quote twice on the happy path.
        const bool needs_cmd_shell = resolves_to_batch(av[0]);
        for (size_t i = 0; i < av.size(); ++i) {
            if (i) cmdline.push_back(' ');
            cmdline += win_quote_arg(av[i]);
        }
        if (needs_cmd_shell)
            cmdline = "cmd.exe /S /C \"" + cmdline + "\"";
    }
    // No trailing "no command specified" arm: the variant has exactly two
    // alternatives, so shell_if() being null means argv_if() is not.
    const std::string prog = opts.shell_if() ? std::string{"cmd.exe"} : (*opts.argv_if())[0];
    return run_via_child({prog}, opts, std::move(cmdline));
#else
    // Exactly two alternatives, so the null check on one IS the other:
    // there is no "neither form set" arm to write, because that state no
    // longer exists. The old chain ended in a `"no command specified"`
    // runtime error for a condition the type now rules out.
    if (const std::string* sh = opts.shell_if()) {
        // Shell form: pass the whole command string as the single sh -c
        // argument. The runner sets up sh "-c" "<cmd>" itself; no
        // additional quoting needed (and we don't want any — the user
        // passed shell syntax expecting it to be parsed verbatim).
        return run_via_child({"sh", "-c", *sh}, opts);
    }
    const std::vector<std::string>& av = *opts.argv_if();
    if (av.empty()) {
        r.started = false; r.start_error = "empty command"; return r;
    }
    // argv form: exec directly with no shell in the loop. Preserves
    // every byte of every arg, which is what callers like git_commit
    // (commit messages with $vars / quotes / newlines) actually need.
    return run_via_child(av, opts);
#endif
}

// ── Convenience wrappers ────────────────────────────────────────────────
//
// For agentty's own helpers (git for checkpoints, keyring, hooks). No live
// progress: these are not tool calls, so nobody is watching a card.

SubprocessResult run_command_s(const std::string& cmd,
                               std::size_t max_bytes,
                               std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.command     = SubprocessOptions::Shell{cmd};
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;
    return Subprocess::run(std::move(opts));
}

SubprocessResult run_argv_s(const std::vector<std::string>& argv,
                            std::size_t max_bytes,
                            std::chrono::seconds timeout) {
    SubprocessOptions opts;
    opts.command     = SubprocessOptions::Argv{argv};
    opts.max_bytes   = max_bytes;
    opts.timeout     = timeout;
    return Subprocess::run(std::move(opts));
}

std::string legacy_format(const SubprocessResult& r, std::chrono::seconds timeout) {
    if (!r.started) return "[" + r.start_error + "]";
    std::string o = r.output;
    if (r.truncated) o += "\n[output truncated]";
    if (r.hard_capped) {
        // It was still producing output when we stopped it, so the useful
        // next step is more time or a background session -- not the hunt for
        // a hang that "timed out" would send the model on.
        o += "\n[stopped at the wall-clock ceiling while still producing "
             "output. It was not stuck. Re-run with a larger `timeout`, or "
             "start it with `process_start` and poll it, which is the right "
             "shape for anything long-running.]";
    } else if (r.timed_out) {
        o += "\n[no output for " + std::to_string(timeout.count())
           + "s, so it was stopped. The clock measures SILENCE, not total "
             "runtime -- a long build that keeps printing is never cut off. "
             "If it is legitimately quiet for a while, raise `timeout`.]";
    } else if (r.exit_code != 0) {
        o += "\n[exit code " + std::to_string(r.exit_code) + "]";
    }
    return o;
}

std::string run_command(const std::string& cmd,
                        std::size_t max_bytes,
                        std::chrono::seconds timeout) {
    return legacy_format(run_command_s(cmd, max_bytes, timeout), timeout);
}

std::string run_argv(const std::vector<std::string>& argv,
                     std::size_t max_bytes,
                     std::chrono::seconds timeout) {
    return legacy_format(run_argv_s(argv, max_bytes, timeout), timeout);
}

} // namespace agentty::tools::util
