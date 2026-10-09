#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/utf8.hpp"
#include "agentty/io/fsm.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// guarded<T> pairs the pipe buffer with its mutex; scope() joins the reader
// helper on every exit path. See jaal/docs/concurrency.md.
#include <maya/runtime.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#  include "agentty/tool/util/exec.hpp"   // run_child: the one supervise loop
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

// Best-effort progress flush throttle. 80 ms keeps the UI responsive without
// drowning the event queue — the worst case (30 KB / flush) is a few hundred
// µs of UTF-8 decode work, negligible against the subprocess wall clock.
constexpr std::chrono::milliseconds kEmitGap{80};

#ifdef _WIN32

// Owns one Win32 HANDLE; closes on scope exit unless released. Move-only.
// Mirrors the POSIX FdGuard — collapses the CreatePipe/CreateFile/
// CreateProcess failure ladders (each previously had to CloseHandle every
// handle acquired so far) into RAII so a new early-return can't leak one.
struct HandleGuard {
    HANDLE h = nullptr;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE x) noexcept : h(x) {}
    HandleGuard(HandleGuard&& o) noexcept : h(std::exchange(o.h, nullptr)) {}
    HandleGuard& operator=(HandleGuard&& o) noexcept {
        if (this != &o) { reset(); h = std::exchange(o.h, nullptr); }
        return *this;
    }
    ~HandleGuard() { reset(); }
    void reset() noexcept {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = nullptr;
    }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(h, nullptr); }
};

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

// CreateProcess-based runner. Redirects the child's stdin to NUL so it
// can't steal keystrokes from the TUI or disturb the console mode. Saves +
// restores the stdin console mode as a belt-and-suspenders guard: a child
// that resets ENABLE_LINE_INPUT / ENABLE_ECHO_INPUT (bash, some shells)
// would otherwise make the next keystroke echo at the cursor instead of
// flowing into the composer.
SubprocessResult run_win32_cmdline(const std::string& cmdline,
                                   const SubprocessOptions& opts) {
    SubprocessResult r;
    HANDLE h_stdin = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved_in_mode = 0;
    bool  have_saved_mode =
        h_stdin != INVALID_HANDLE_VALUE && ::GetConsoleMode(h_stdin, &saved_in_mode);

    struct Restore {
        HANDLE h; DWORD mode; bool active;
        ~Restore() { if (active) ::SetConsoleMode(h, mode); }
    } restore{h_stdin, saved_in_mode, have_saved_mode};

    HANDLE rd_raw = nullptr, wr_raw = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    // 64 KiB pipe buffer instead of the default (4 KiB). A chatty child
    // (cmake/clang/msbuild, test runners, npm) fills a 4 KiB pipe in a
    // single printf and blocks on write until our reader thread drains it,
    // serializing the child's stdout with our UI loop. 64 KiB soaks a full
    // compile-step's output so the child keeps running while we read.
    if (!::CreatePipe(&rd_raw, &wr_raw, &sa, 64 * 1024)) {
        r.started = false; r.start_error = "CreatePipe failed"; return r;
    }
    // Own both pipe ends via RAII: every early return below closes whatever
    // is still live, no hand-written CloseHandle ladder.
    HandleGuard rd{rd_raw};
    HandleGuard wr{wr_raw};
    ::SetHandleInformation(rd.h, HANDLE_FLAG_INHERIT, 0);

    HandleGuard nul{::CreateFileW(L"NUL", GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr)};
    if (nul.h == INVALID_HANDLE_VALUE) {
        r.started = false; r.start_error = "CreateFile(NUL) failed"; return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = nul.h;
    si.hStdOutput = wr.h;
    si.hStdError  = wr.h;

    // CreateProcessW takes a mutable LPWSTR — widen the UTF-8 cmdline and
    // give it a writable buffer.
    std::wstring wcmd = utf8_to_wide(cmdline);
    std::vector<wchar_t> mutable_cmdline(wcmd.begin(), wcmd.end());
    mutable_cmdline.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, mutable_cmdline.data(),
                               nullptr, nullptr,
                               TRUE,
                               CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                               nullptr, nullptr,
                               &si, &pi);
    // The child has its own dup'd copies now; drop the parent's write end +
    // NUL handle (RAII no-ops them at scope exit too, but eager release
    // matches the original ordering so the child's EOF is observable).
    wr.reset();
    nul.reset();
    if (!ok) {
        DWORD e = ::GetLastError();
        r.started = false;
        r.start_error = "CreateProcess failed (" + std::to_string(e) + ")";
        return r;   // ~rd closes the read end
    }
    // The read end is handed to the reader thread below and force-closed
    // after join(); take it out of RAII so the guard's dtor doesn't
    // double-close. `rd` (the raw HANDLE) is the owner from here on.
    HANDLE rd_h = rd.release();

    // Reader thread drains the pipe into `shared_buf` under a mutex. The
    // previous single-thread loop read synchronously with no deadline —
    // a child that spawned a detached grandchild (and so kept the write
    // end of the pipe open after its own exit) or that wedged without
    // emitting output would pin ReadFile forever, because the outer
    // WaitForSingleObject only ran *after* the read loop closed.
    //
    // With the reader split off:
    //   • main thread does WaitForSingleObject(pi.hProcess, …) with the
    //     full timeout in short chunks, flushing progress between chunks;
    //   • timeout fires → TerminateProcess → child's write end closes →
    //     reader's ReadFile returns 0 → reader thread exits cleanly;
    //   • grandchild-keeps-pipe-alive edge case → after the terminate,
    //     we CloseHandle(rd) to force-unblock the reader.
    // The pipe buffer the reader fills and the poll loop samples.
    //
    // This used to be four loose variables plus a `std::mutex buf_mu` that
    // every reader had to remember to take. The grouping lived in the
    // variable NAMES (`shared_*`) and in review discipline: nothing stopped
    // a later edit from reading shared_total without the lock, and that bug
    // would be a torn size_t on a timeout path nobody tests.
    //
    // maya::guarded<T> makes the mutex and the data one thing. The state is
    // unreachable except inside a with()/read() body, so "took the lock"
    // stops being something to remember. The callable must also be
    // CAPTURELESS — which is jaal's deadlock rule, since a capture could
    // name a second lock — so everything a body needs is passed as an
    // argument, and the compiler checks the result doesn't point back into
    // the protected data.
    struct PipeBuf {
        std::ostringstream buf;
        std::size_t        total     = 0;
        bool               truncated = false;
    };
    maya::guarded<PipeBuf> shared;
    std::atomic<bool>      reader_done{false};

    auto snapshot = [&] {
        return shared.read([](const PipeBuf& s) { return s.buf.str(); });
    };
    auto total_snapshot = [&] {
        return shared.read([](const PipeBuf& s) { return s.total; });
    };

    auto now_ms = [] { return std::chrono::steady_clock::now(); };

    // Everything that depends on the reader lives inside a maya::scope.
    //
    // Before, the reader was a bare `std::thread reader(...)` joined ~90
    // lines later, with `opts.on_progress(...)` — a caller-supplied callback
    // — invoked twice in between. If that callback threw, ~thread ran on a
    // joinable thread and the process called std::terminate: no stack, no
    // message, and on Windows not even a core. The reader also captured the
    // buffer and `opts` by reference, which is only sound because of a join
    // nothing guaranteed we would reach.
    //
    // scope() joins on every exit path, so the [&] captures are now
    // warranted rather than hoped-for, and a throwing on_progress unwinds
    // normally with the reader already stopped.
    maya::scope([&](maya::nursery& n) {
    auto reader = n.spawn([&, rd_h] {
        char tmp[4096];
        for (;;) {
            DWORD n_read = 0;
            if (!::ReadFile(rd_h, tmp, sizeof(tmp), &n_read, nullptr) || n_read == 0) break;
            // Captureless body, and the bytes arrive as an OWNED string_view
            // over a std::string rather than a raw char* — jaal rejects the
            // pointer, correctly: a pointer argument is how a lock body
            // reaches data whose lifetime the lock does not cover.
            shared.with(
                [](PipeBuf& s, std::string chunk, std::size_t cap) {
                    if (s.truncated) return;
                    const std::size_t room = (s.total < cap) ? cap - s.total : 0;
                    const std::size_t w    = chunk.size() < room ? chunk.size() : room;
                    s.buf.write(chunk.data(), (std::streamsize)w);
                    s.total += w;
                    if (w < chunk.size()) s.truncated = true;
                },
                std::string(tmp, (std::size_t)n_read), (std::size_t)opts.max_bytes);
        }
        reader_done.store(true, std::memory_order_release);
    });

    // Idle deadline: same semantics as the POSIX path — we cap *silence*,
    // not total wall-clock from spawn.  A child that's actively writing
    // stdout/stderr keeps rolling the deadline forward, so a long but
    // chatty build never trips the watchdog; only a stuck child that goes
    // quiet for `opts.timeout` seconds gets terminated.  Activity is
    // detected by snapshotting the reader's `shared_total` byte counter
    // under the buffer mutex — when it grows between iterations, the
    // window resets.
    const auto idle_window = (opts.timeout.count() > 0)
        ? std::chrono::milliseconds(opts.timeout.count() * 1000)
        : std::chrono::milliseconds::zero();
    const bool has_idle_window = idle_window.count() > 0;
    auto idle_deadline = has_idle_window
        ? now_ms() + idle_window
        : std::chrono::steady_clock::time_point::max();
    // The same absolute ceiling the POSIX path keeps: output rolls the idle
    // window forward forever, so without this a chatty runaway is never
    // reaped. 20x idle, floored at 10 min.
    const auto hard_window = [&]() -> std::chrono::milliseconds {
        if (opts.hard_timeout.count() > 0)
            return std::chrono::milliseconds(opts.hard_timeout.count() * 1000);
        if (!has_idle_window) return std::chrono::milliseconds::zero();
        return std::max<std::chrono::milliseconds>(
            idle_window * 20,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::minutes{10}));
    }();
    const bool has_hard_window = hard_window.count() > 0;
    const auto hard_deadline = has_hard_window
        ? now_ms() + hard_window
        : std::chrono::steady_clock::time_point::max();
    auto last_emit = now_ms();
    std::size_t last_total_seen = 0;

    bool timed_out = false;
    bool hard_capped = false;
    bool cancelled = false;
    for (;;) {
        auto now = now_ms();
        if (opts.stop_requested && opts.stop_requested()) {
            cancelled = true;
            break;
        }
        // Reset the idle window if the reader thread has appended bytes
        // since our last check.  Snapshot is cheap (one mutex acquire +
        // size_t copy) and we only do it once per outer iteration.
        if (has_idle_window) {
            const auto t = total_snapshot();
            if (t > last_total_seen) {
                last_total_seen = t;
                idle_deadline = now + idle_window;
            }
        }
        auto remaining_deadline = has_idle_window
            ? std::chrono::duration_cast<std::chrono::milliseconds>(idle_deadline - now)
            : std::chrono::milliseconds::max();
        if (has_idle_window && remaining_deadline.count() <= 0) {
            timed_out = true;
            break;
        }
        if (has_hard_window && now >= hard_deadline) {
            timed_out   = true;
            hard_capped = true;
            break;
        }
        if (has_hard_window) {
            const auto to_hard = std::chrono::duration_cast<std::chrono::milliseconds>(
                hard_deadline - now);
            if (to_hard < remaining_deadline) remaining_deadline = to_hard;
        }
        auto to_emit = kEmitGap - std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_emit);
        if (to_emit.count() < 0) to_emit = std::chrono::milliseconds{0};
        auto sleep_ms = std::min<std::chrono::milliseconds>(
            to_emit, has_idle_window ? remaining_deadline : std::chrono::milliseconds{1000});

        DWORD w = ::WaitForSingleObject(
            pi.hProcess,
            (DWORD)std::max<std::chrono::milliseconds::rep>(sleep_ms.count(), 0));
        if (w == WAIT_OBJECT_0) break;   // child exited

        auto after = now_ms();
        if (opts.on_progress && (after - last_emit) >= kEmitGap) {
            opts.on_progress(clean_capture(snapshot()));
            last_emit = after;
        }
    }

    DWORD exit_code = 0;
    if (timed_out || cancelled) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);
        r.timed_out = timed_out;
        r.hard_capped = hard_capped;
    } else {
        ::GetExitCodeProcess(pi.hProcess, &exit_code);
        r.exit_code = (int)exit_code;
    }

    // Give the reader a grace window to drain the remaining pipe bytes
    // after the child exited (its write end closed → ReadFile is returning).
    // Grandchildren that inherited the pipe can hold it open past the
    // parent's death — if the reader is still blocked, force-close rd.
    const auto grace_deadline = now_ms() + std::chrono::milliseconds(500);
    while (!reader_done.load(std::memory_order_acquire)
           && now_ms() < grace_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::CloseHandle(rd_h);   // safe: even if reader is mid-ReadFile, it returns false
    reader.join();         // explicit: surfaces a reader exception here

    if (opts.on_progress) {
        opts.on_progress(clean_capture(snapshot()));
    }
    });   // maya::scope — the reader is joined by here on EVERY path above

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    // The reader has been joined, so this is single-threaded now — but go
    // through the guard anyway rather than reaching around it. One accessor
    // means a future edit that moves this line back inside the scope is
    // still correct.
    r.truncated = shared.read([](const PipeBuf& s) { return s.truncated; });
    r.output    = clean_capture(snapshot());
    return r;
}

#else // POSIX

// One supervise loop for every child agentty runs: run_child (exec.cpp) on
// jaal's posix_process. This only maps the options across.
SubprocessResult run_posix(std::vector<std::string> argv, const SubprocessOptions& opts) {
    ChildRun run;
    run.argv = std::move(argv);
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

#endif

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
    return run_win32_cmdline(cmdline, opts);
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
        return run_posix({"sh", "-c", *sh}, opts);
    }
    const std::vector<std::string>& av = *opts.argv_if();
    if (av.empty()) {
        r.started = false; r.start_error = "empty command"; return r;
    }
    // argv form: exec directly with no shell in the loop. Preserves
    // every byte of every arg, which is what callers like git_commit
    // (commit messages with $vars / quotes / newlines) actually need.
    return run_posix(av, opts);
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
