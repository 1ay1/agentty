// cmd_code_block_run.cpp — the EFFECT half of the Ctrl+G code-block flow.
//
// update/code_blocks.cpp is the reducer: it decides WHICH block runs and
// returns cmd::run_code_block(...). This file is what that Cmd does when the
// runtime runs it — fork/exec on the real tty with a tee (POSIX), or the
// captured subprocess runner (Windows). It lived inside the reducer file,
// which put ~500 lines of fork, termios, signals and clock reads under
// update/: not called from a fold (it always ran as a task), but readable
// as if it were, and invisible to the elm_purity lint's file boundary.
//
// Nothing here is reached except through the Cmd. See update/code_blocks.cpp
// for the user-facing design (interactive-first, capture to the composer
// only on request, Windows degradation).

#include "agentty/runtime/app/cmd_code_block_run.hpp"

#include "agentty/runtime/app/update/internal.hpp"

#if !defined(_WIN32)
    #include <csignal>
    #include <cstdio>
    #include <ctime>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <sys/wait.h>
    #include <termios.h>
    #include <unistd.h>
#else
    #include <atomic>
    #include <chrono>
    #include <cstdio>
    #include <io.h>
    #include <thread>
#endif

#include <string>
#include <utility>

#include <maya/style/theme.hpp>

#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/runtime/win_shell_encode.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace agentty::app::cmd {

namespace cbp = agentty::code_blocks;

namespace {
// the real terminal with a stdout/stderr tee — fully interactive, output
// captured. Windows: non-interactive captured runner (subprocess), same
// as the bash tool.
#if !defined(_WIN32)

// Run `command` via /bin/sh -c on the REAL terminal (we're inside maya's
// suspend: cooked tty, TUI escapes torn down). stdin is inherited — the
// actual tty — so line editing / password reads behave; sudo talks to
// /dev/tty directly either way. stdout+stderr are dup2'd onto a pipe the
// parent tees: each read chunk is written straight to the tty (live
// output) and appended to the capture buffer. Returns the finished Msg.
// Small helpers so the transcript looks the same whether or not stdout is
// a real terminal. Colour/heartbeat are TTY-only (piped output stays clean
// for `agentty ... | tee`), but the command echo + status always print.
namespace runner_ui {
    // Colour here cannot go through the theme, and that is not an oversight:
    // maya is SUSPENDED at this point (cooked tty, TUI torn down) and we are
    // writing bytes straight at the terminal, so there is no canvas, no
    // style pool and no frame to paint into. These are the only hand-written
    // escapes in agentty for exactly that reason.
    //
    // What they MUST still honour is whether the user wants colour at all.
    // This used to be a bare isatty() check, which silently ignored NO_COLOR
    // and TERM=dumb — so `NO_COLOR=1 agentty` stayed clean everywhere except
    // here, which is the one place a user cannot theme their way out of.
    // maya::theme::detect_tier() is the same detector the renderer uses, so
    // the answer is consistent across the suspend boundary.
    inline bool tty() {
        static const bool on = ::isatty(STDOUT_FILENO) == 1
            && maya::theme::detect_tier(/*tty=*/true)
                   != maya::theme::ColorTier::Mono;
        return on;
    }
    inline const char* dim()   { return tty() ? "\x1b[2m"  : ""; }
    inline const char* bold()  { return tty() ? "\x1b[1m"  : ""; }
    inline const char* green() { return tty() ? "\x1b[32m" : ""; }
    inline const char* red()   { return tty() ? "\x1b[31m" : ""; }
    inline const char* yellow(){ return tty() ? "\x1b[33m" : ""; }
    inline const char* cyan()  { return tty() ? "\x1b[36m" : ""; }
    inline const char* reset() { return tty() ? "\x1b[0m"  : ""; }
    inline void emit(const std::string& s) {
        (void)!::write(STDOUT_FILENO, s.data(), s.size());
    }
    // Clear the current line (used to wipe an in-place heartbeat before
    // real output or the final status lands on top of it).
    inline void clear_line() { if (tty()) emit("\r\x1b[2K"); }
    // Set / restore the terminal window title (OSC 2). This is an
    // ALWAYS-ON elapsed-time readout that lives in the titlebar, so it
    // updates every second even while output is streaming without ever
    // touching the scrollback area. No-op off a TTY.
    inline void set_title(const std::string& t) {
        if (tty()) emit("\x1b]2;" + t + "\x07");
    }
    inline void restore_title() { if (tty()) emit("\x1b]2;\x07"); }

    // ── Pinned bottom-line status (DECSTBM) ──────────────────────────────
    // A universal fallback for terminals/multiplexers that drop the OSC-2
    // window title: reserve the bottom screen row as a fixed status line so
    // the elapsed-time readout is ALWAYS visible even while output streams.
    // Output written by the tee loop scrolls within rows 1..h-1 (the
    // DECSTBM margin); we repaint row h out-of-band. Every escape here is
    // TTY-gated and fully torn down by end_status(), so piped output and
    // the captured buffer never see any of it.
    inline int term_rows() {
        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
            return ws.ws_row;
        return 0;   // unknown / too small — caller disables the status line
    }
    // Enter status mode: set the scroll region to rows 1..(rows-1), leaving
    // row `rows` reserved. Cursor is parked at the top of the scroll region.
    inline void begin_status(int rows) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[1;" + std::to_string(rows - 1) + "r";  // DECSTBM margin
        s += "\x1b[" + std::to_string(rows - 1) + ";1H";  // park cursor
        emit(s);
    }
    // Repaint the reserved bottom row with `text` (already styled), then
    // return the cursor into the scroll region so output continues above.
    inline void paint_status(int rows, const std::string& text) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[s";                                    // save cursor
        s += "\x1b[" + std::to_string(rows) + ";1H";      // to status row
        s += "\x1b[2K";                                   // clear it
        s += text;
        s += "\x1b[u";                                    // restore cursor
        emit(s);
    }
    // Leave status mode: reset the scroll region to full screen and clear
    // the reserved row so the transcript ends clean.
    inline void end_status(int rows) {
        if (!tty() || rows < 3) return;
        std::string s;
        s += "\x1b[r";                                    // reset DECSTBM
        s += "\x1b[" + std::to_string(rows) + ";1H";
        s += "\x1b[2K";
        s += "\x1b[" + std::to_string(rows - 1) + ";1H";
        emit(s);
    }
    // Hand the child a clean screen. maya's inline-mode suspend only emits
    // "\r\n" on teardown — it moves the cursor onto a fresh line but leaves
    // the whole last TUI frame (transcript, composer, picker) still painted
    // ABOVE it. The run header + child output then scroll up INTO that stale
    // frame and overlap it ("text already there that overlaps when you
    // type"). Clear the visible viewport and home the cursor so the child
    // starts on a blank screen.
    //
    // ED2 (\x1b[2J) clears the VIEWPORT only; scrollback is untouched (that
    // needs ED3 / \x1b[3J, which we deliberately do NOT send) so the
    // conversation history the user scrolled through stays in native
    // scrollback. TTY-gated, so piped/captured output never sees it.
    inline void reset_screen() {
        if (!tty()) return;
        emit("\x1b[2J\x1b[H");   // ED2 clear viewport + cursor home
    }
}

[[nodiscard]] CodeBlockRunFinished run_on_real_tty(const std::string& command) {
    namespace ui = runner_ui;
    CodeBlockRunFinished fin;
    fin.command = command;

    // Hand the child a CLEAN screen. maya's inline suspend leaves the last
    // TUI frame painted above the cursor; without this the header and child
    // output scroll up into it and overlap. reset_screen() clears the
    // viewport (scrollback preserved) and homes the cursor.
    ui::reset_screen();

    // Framed run header: a clearly-delimited banner so the user can see at
    // a glance WHERE the run started (the TUI just tore down, so a bare
    // "$ cmd" line was easy to lose) and, crucially, that Ctrl-C stops it.
    // The command still reads like a shell prompt so the transcript is
    // copy-paste-faithful.
    {
        std::string header;
        header += ui::dim();
        // No leading \n — reset_screen() already homed the cursor to row 1.
        header += "\xe2\x95\xad\xe2\x94\x80 running \xe2\x94\x80 Ctrl-C to stop \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80";
        header += ui::reset();
        header += "\n";
        header += ui::cyan();
        header += "$ ";
        header += ui::reset();
        header += ui::bold();
        header += command;
        header += ui::reset();
        header += "\n";
        ui::emit(header);
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        fin.output    = "[failed to start: pipe() failed]";
        fin.exit_code = -1;
        return fin;
    }
    // CLOEXEC both ends: another thread's fork/exec (MCP server spawn, tool
    // subprocess) must not inherit this capture pipe — a long-lived sibling
    // child pinning fds[1] open would starve the read loop of EOF forever.
    // Our own child's dup2 onto stdout below clears the flag on the duplicate.
    (void)::fcntl(fds[0], F_SETFD, ::fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
    (void)::fcntl(fds[1], F_SETFD, ::fcntl(fds[1], F_GETFD) | FD_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]); ::close(fds[1]);
        fin.output    = "[failed to start: fork() failed]";
        fin.exit_code = -1;
        return fin;
    }

    if (pid == 0) {
        // ── Child ── default signal dispositions (the parent ignores
        // SIGINT/SIGQUIT below; we must NOT inherit that or Ctrl+C
        // couldn't stop the command).
        ::signal(SIGINT,  SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }

    // ── Parent ── classic system() semantics: ignore INT/QUIT while the
    // child runs so Ctrl+C kills only the command, not agentty.
    struct sigaction ign{}, old_int{}, old_quit{};
    ign.sa_handler = SIG_IGN;
    ::sigaction(SIGINT,  &ign, &old_int);
    ::sigaction(SIGQUIT, &ign, &old_quit);

    ::close(fds[1]);
    // Tee loop: live to the tty, captured to the buffer. Bounded capture
    // (2 MB) so a runaway command can't OOM the composer — the SCREEN
    // still shows everything; only the buffer stops growing.
    //
    // We poll(2) with a 1s timeout instead of a bare blocking read so a
    // SILENT command (npm install fetching, a network wait, a `sleep`)
    // still shows a heartbeat — "⠿ running… 12s (Ctrl-C to stop)" redrawn
    // in place — instead of a frozen-looking screen. The heartbeat is
    // TTY-only and is wiped the instant real output or the exit status
    // arrives, so it never pollutes the captured buffer or piped output.
    constexpr std::size_t kCaptureMax = 2u * 1024 * 1024;
    bool truncated = false;
    char buf[8192];
    const auto started = ::time(nullptr);
    long last_status_secs = -1;
    int  spin = 0;
    static constexpr const char* kSpin[] = {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
    // Short command label for the timers (first token, clipped).
    std::string label = command.substr(0, command.find_first_of(" \t\n"));
    if (label.size() > 24) label.resize(24);

    // Two always-on elapsed readouts, belt-and-suspenders so SOMETHING is
    // visible regardless of terminal quirks:
    //   1. OSC-2 window title — non-intrusive, but some multiplexers drop it.
    //   2. A DECSTBM-pinned bottom status row — universal; output scrolls
    //      above it. Enabled only when we can read the terminal height.
    const int rows = ui::term_rows();
    ui::begin_status(rows);
    auto tick = [&] {
        const long secs = static_cast<long>(::time(nullptr) - started);
        if (secs == last_status_secs) return;
        last_status_secs = secs;
        ui::set_title("● " + std::to_string(secs) + "s — " + label + " — agentty");
        if (rows >= 3) {
            std::string bar = ui::dim();
            bar += kSpin[spin++ % 8];
            bar += " running… " + std::to_string(secs) + "s · " + label
                 + " · Ctrl-C to stop";
            bar += ui::reset();
            ui::paint_status(rows, bar);
        }
    };
    tick();
    // Exit conditions, in the order they matter:
    //   • EOF on the pipe — the normal case (child exited, no survivors).
    //   • CHILD DEAD + PIPE QUIET — the hang-proof case. EOF requires
    //     EVERY write end closed, and a grandchild the command left
    //     behind (`npm run dev &`, a daemonizing build, a watcher)
    //     inherits stdout/stderr and can hold the pipe open FOREVER
    //     after the child exits. Without this arm the TUI stayed
    //     suspended indefinitely — the reported "^G sometimes hangs".
    //     So: reap the child without blocking inside the loop; once it
    //     is dead and a full poll interval passes with no data, stop.
    //     A still-attached background survivor that writes later gets
    //     EPIPE/SIGPIPE — the price of restoring the UI; a command that
    //     WANTS a survivor should redirect its output (`… > log &`).
    bool child_reaped = false;
    int  status = 0;
    for (;;) {
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 1000);
        tick();   // refresh timers whether or not output arrived this tick
        if (!child_reaped
            && ::waitpid(pid, &status, WNOHANG) == pid)
            child_reaped = true;
        if (pr == 0) {
            if (child_reaped) break;   // dead + quiet ≥ 1s → done
            continue;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        (void)!::write(STDOUT_FILENO, buf, static_cast<std::size_t>(n));
        if (fin.output.size() < kCaptureMax) {
            const auto room = kCaptureMax - fin.output.size();
            fin.output.append(buf, std::min(static_cast<std::size_t>(n), room));
            if (static_cast<std::size_t>(n) > room) truncated = true;
        } else {
            truncated = true;
        }
    }
    ui::end_status(rows);
    ui::restore_title();
    ::close(fds[0]);

    if (!child_reaped)
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::sigaction(SIGINT,  &old_int,  nullptr);
    ::sigaction(SIGQUIT, &old_quit, nullptr);

    // Distinguish a user interrupt (Ctrl-C / Ctrl-\) from a genuine
    // command failure — "failed exit 130" reads like the command broke
    // when the user actually stopped it on purpose.
    bool interrupted = false;
    if (WIFEXITED(status)) {
        fin.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        interrupted = (sig == SIGINT || sig == SIGQUIT);
        fin.exit_code = 128 + sig;
    } else {
        fin.exit_code = -1;
    }
    if (truncated) fin.output += "\n[capture truncated at 2 MB — full output was shown on screen]";

    // Completion footer: a colour-coded status line so success/failure/
    // interrupt is unmistakable at a glance, with elapsed time.
    {
        const long secs = static_cast<long>(::time(nullptr) - started);
        const bool ok = (fin.exit_code == 0);
        std::string tail = "\n";
        if (interrupted)   { tail += ui::yellow(); tail += "╰─ ■ stopped"; }
        else if (ok)       { tail += ui::green();  tail += "╰─ ✓ done"; }
        else               { tail += ui::red();    tail += "╰─ ✗ failed"; }
        tail += ui::reset();
        tail += ui::dim();
        tail += "  exit " + std::to_string(fin.exit_code)
              + "  ·  " + std::to_string(secs) + "s";
        tail += ui::reset();
        tail += "\n";
        ui::emit(tail);
    }

    // Keypress-to-continue: hold the transcript on screen until the user
    // acknowledges, so a fast command's output isn't repainted away before
    // it can be read. Only when stdout AND stdin are a real terminal (so
    // we can actually read a key); piped/redirected runs skip it. The
    // prompt is on its own line and cleared afterward so the restored TUI
    // starts on a clean row. Any key continues.
    if (ui::tty() && ::isatty(STDIN_FILENO) == 1) {
        std::string prompt = ui::dim();
        prompt += "   press any key to return to agentty…";
        prompt += ui::reset();
        ui::emit(prompt);
        // Best-effort raw single-key read: disable canonical mode + echo
        // so a lone keypress (no Enter) continues, then restore.
        struct termios oldt{}, raw{};
        const bool have_termios = (::tcgetattr(STDIN_FILENO, &oldt) == 0);
        if (have_termios) {
            raw = oldt;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        char c;
        // EINTR retries; EOF (0) and real errors (EIO after a hangup) fall
        // through to the restore path — this gate can stall only for a live
        // tty with a human who hasn't pressed a key yet, which is its job.
        while (::read(STDIN_FILENO, &c, 1) < 0 && errno == EINTR) {}
        if (have_termios) ::tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        ui::clear_line();
    }
    return fin;
}

[[nodiscard]] Cmd run_block_cmd(std::string command, cbp::BlockShell /*shell*/) {
    return Cmd::suspend(
        [cmd = std::move(command)]() -> Msg {
            return Msg{run_on_real_tty(cmd)};
        });
}

#else  // _WIN32 — non-interactive fallback via the shared subprocess runner

#include <maya/runtime.hpp>

// Wrap the block body for the chosen Windows interpreter. cmd.exe is the
// default shell of run_command_s (it wraps in `cmd.exe /S /C "..."`), so
// a Cmd block passes through verbatim. A PowerShell block is handed to
// powershell -NoProfile -Command; we base64-encode it via -EncodedCommand
// so arbitrary quoting/newlines survive the cmd.exe wrapper intact
// (nested quotes through cmd /C are the classic breakage). Multi-line
// scripts Just Work because the whole body is one encoded argument.
[[nodiscard]] std::string wrap_for_windows_shell(cbp::BlockShell shell,
                                                 const std::string& body) {
    if (shell != cbp::BlockShell::PowerShell) return body;  // Cmd: verbatim
    // PowerShell -EncodedCommand contract (base64 of UTF-16LE) lives in a
    // shared, platform-independent header so it can be verified off Windows
    // — the header carries a compile-time known-answer static_assert block
    // that fails THIS build if the transform ever regresses.
    return win_shell::powershell_command(body);
}

// The task BODY for the Windows runner. jaal requires a task body to capture
// nothing (`TaskBody<Body, Msg, Args...>` resolves to a plain function
// pointer), so everything it needs arrives as Sendable arguments after the
// stop_token. This used to be a capturing lambda, which is why the msys2 leg
// — the only CI lane with a compiler new enough to instantiate jaal's checks
// — failed with "a task body must not capture anything" while MSVC reported
// the same thing as a Sendable static_assert.
//
// The console-echo helper below is also deliberately NOT named `out`: the
// Sink parameter owns that name, and shadowing it made `out.send(...)`
// resolve to the echo lambda.
static void run_block_body(maya::Sink<Msg> out, std::stop_token stop,
                           std::string cmd, cbp::BlockShell shell) {
    const std::string wrapped = wrap_for_windows_shell(shell, cmd);

    // Windows parity for "what's happening while it runs": the
    // shared subprocess runner (run_command_s) is blocking and
    // non-streaming — output only appears in the Result card at the
    // end — so we can't tee live. But we CAN show the run is alive:
    // print a header, spin a background thread that ticks an
    // elapsed-time heartbeat (OSC-2 title + an in-place line;
    // modern Windows Terminal / conhost with VT processing render
    // both), run the command, then a footer. All decoration is
    // console-only and never enters the captured buffer.
    const bool con = (::_isatty(_fileno(stdout)) != 0);
    auto echo = [](const std::string& s) {
        std::fputs(s.c_str(), stdout); std::fflush(stdout);
    };
    if (con) {
        echo("\x1b[2m\n╭─ running ─ (output shown when it finishes) ─────\x1b[0m\n"
             "\x1b[36m$ \x1b[0m\x1b[1m" + cmd + "\x1b[0m\n");
    }

    std::string label = cmd.substr(0, cmd.find_first_of(" \t\n"));
    if (label.size() > 24) label.resize(24);

    // The heartbeat runs in a maya::scope, not a bare std::thread.
    //
    // The old shape was `std::thread ticker; ... ticker.join()` with the
    // blocking run_command_s between them. Two problems, both invisible until
    // they bite: if anything in that window threw, ~thread ran on a joinable
    // thread and the process called std::terminate (no stack, no message);
    // and the helper captured `done_flag` and `label` by reference, which is
    // only safe because of that same join nobody was guaranteed to reach.
    //
    // scope() joins every helper on EVERY exit path — normal return, early
    // return, exception — which is what makes the [&] capture of locals
    // legitimate rather than lucky. There is also no detach() to reach for,
    // so the helper cannot be made to outlive the frame it borrows from.
    //
    // The task's own stop_token is passed in as the parent, so Esc (or
    // shutdown) cancels the heartbeat through the same path as everything
    // else instead of needing its own flag. done_flag stays for the normal
    // "the command finished" stop, which is not a cancellation.
    struct Outcome {
        tools::util::SubprocessResult r;
        long                          secs = 0;
    };

    auto outcome = maya::scope(stop, [&](maya::nursery& n) {
        std::atomic<bool> done_flag{false};

        // The helper takes its own stop_token: jaal derives it from the
        // scope's, which in turn derives from the task's, so Esc reaches the
        // spinner without anybody wiring a second flag.
        auto beat = n.spawn([&done_flag, &label, con](std::stop_token st) {
            if (!con) return;
            static constexpr const char* kSpin[] =
                {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
            int spin = 0;
            const auto start = std::chrono::steady_clock::now();
            while (!done_flag.load(std::memory_order_relaxed)
                   && !st.stop_requested()) {
                const long secs = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start).count());
                std::string s = "\x1b]2;\u25cf " + std::to_string(secs)
                              + "s \u2014 " + label + " \u2014 agentty\x07";
                s += "\r\x1b[2K\x1b[2m" + std::string(kSpin[spin++ % 8])
                   + " running\u2026 " + std::to_string(secs) + "s\x1b[0m";
                std::fputs(s.c_str(), stdout); std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });

        const auto start = std::chrono::steady_clock::now();
        Outcome o;
        o.r = tools::util::run_command_s(wrapped);
        o.secs = static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count());

        done_flag.store(true, std::memory_order_relaxed);
        beat.join();          // explicit, so a throwing helper surfaces here
        return o;
    });

    auto r          = std::move(outcome.r);
    const long secs = outcome.secs;

    CodeBlockRunFinished fin;
    fin.command   = cmd;   // show the ORIGINAL body in the card
    fin.timed_out = r.timed_out;
    if (!r.started) {
        fin.output    = "[failed to start: " + r.start_error + "]";
        fin.exit_code = -1;
    } else {
        fin.output    = std::move(r.output);
        fin.exit_code = r.exit_code;
        if (r.truncated) fin.output += "\n[output truncated]";
    }

    if (con) {
        const bool ok = (fin.exit_code == 0 && !r.timed_out);
        std::string tail = "\r\x1b[2K\x1b]2;\x07";   // wipe hb + restore title
        tail += ok ? "\x1b[32m╰─ ✓ done"
             : r.timed_out ? "\x1b[33m╰─ ■ timed out"
             : "\x1b[31m╰─ ✗ failed";
        tail += "\x1b[0m\x1b[2m  exit " + std::to_string(fin.exit_code)
              + "  ·  " + std::to_string(secs) + "s\x1b[0m\n";
        echo(tail);
    }

    out.send(Msg{std::move(fin)});
}

[[nodiscard]] Cmd run_block_cmd(std::string command, cbp::BlockShell shell) {
    // Body is a plain function; the command and shell ride along as Sendable
    // arguments, which is what jaal's task contract asks for.
    return Cmd::task_isolated(run_block_body, std::move(command), shell);
}

#endif
} // namespace

Cmd run_code_block(std::string command, cbp::BlockShell shell) {
    return run_block_cmd(std::move(command), shell);
}

} // namespace agentty::app::cmd
