// codeblock_update — reducer for the Ctrl+G code-block picker (the "run
// what the AI just suggested" flow). Open scans the newest assistant
// message for fenced blocks; Select suspends the TUI and runs the chosen
// block INTERACTIVELY on the real terminal — sudo password prompts work,
// output streams live — while a tee captures every output byte;
// RunFinished opens the Result card where the user decides what happens
// to the captured copy: attach to the composer as an Output chip, copy
// it clean, or discard.
//
// Deliberate scope decisions:
//   • Interactive-first: the run happens under maya's Cmd::suspend — the
//     TUI tears down to a cooked tty, the child inherits the REAL
//     terminal for stdin (sudo reads the password from /dev/tty, which
//     stays the real tty regardless of our stdout pipe), and stdout+
//     stderr flow through a tee pipe: every byte hits the user's screen
//     live AND lands in the capture buffer. Ctrl+C uses classic
//     system() semantics — the parent ignores SIGINT/SIGQUIT for the
//     duration, the child takes the default action and dies; agentty
//     resumes cleanly.
//   • The captured output only reaches the COMPOSER when the user
//     presses `a` on the Result card — it lands as an Output attachment
//     (chip), not a conversation message: the same collapse/expand
//     contract as a big paste. The user annotates and submits when (and
//     if) they want the model to see it.
//   • Only shell-ish blocks are runnable (see is_shell_language). For a
//     python/js block Select shows a toast; Edit / Copy still work.
//   • Opening is gated on an idle session: mid-stream the message list
//     is in flux and the "latest assistant reply" is still growing.
//   • Windows: no fork/tcsetattr — falls back to the non-interactive
//     captured runner (same one the bash tool uses). sudo isn't a
//     Windows concept anyway; the honest degradation.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <utility>

#if !defined(_WIN32)
    #include <csignal>
    #include <cstdio>
    #include <ctime>
    #include <poll.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include <maya/core/overload.hpp>

#include "agentty/runtime/code_block_picker.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace agentty::app::detail {

using maya::overload;
namespace cbp = agentty::code_block_picker;

namespace {

// Newest assistant message that yields at least one fenced block.
// Document order within the message is preserved (block 1 = topmost),
// matching how the user visually indexes the reply on screen.
[[nodiscard]] std::vector<CodeBlock> latest_assistant_blocks(const Model& m) {
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role != Role::Assistant) continue;
        if (it->text.empty()) continue;
        auto blocks = cbp::extract_code_blocks(it->text);
        if (!blocks.empty()) return blocks;
        // Keep walking: an assistant turn with prose but no fences
        // shouldn't mask an earlier reply that HAS runnable blocks
        // (common shape: reply N has the commands, reply N+1 is a
        // short "let me know how it goes" follow-up).
    }
    return {};
}

// Fire the selected block. POSIX: suspend the TUI and run the child on
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
    inline bool tty() { return ::isatty(STDOUT_FILENO) == 1; }
    inline const char* dim()   { return tty() ? "\x1b[2m"  : ""; }
    inline const char* bold()  { return tty() ? "\x1b[1m"  : ""; }
    inline const char* green() { return tty() ? "\x1b[32m" : ""; }
    inline const char* red()   { return tty() ? "\x1b[31m" : ""; }
    inline const char* cyan()  { return tty() ? "\x1b[36m" : ""; }
    inline const char* reset() { return tty() ? "\x1b[0m"  : ""; }
    inline void emit(const std::string& s) {
        (void)!::write(STDOUT_FILENO, s.data(), s.size());
    }
    // Clear the current line (used to wipe an in-place heartbeat before
    // real output or the final status lands on top of it).
    inline void clear_line() { if (tty()) emit("\r\x1b[2K"); }
}

[[nodiscard]] CodeBlockRunFinished run_on_real_tty(const std::string& command) {
    namespace ui = runner_ui;
    CodeBlockRunFinished fin;
    fin.command = command;

    // Framed run header: a clearly-delimited banner so the user can see at
    // a glance WHERE the run started (the TUI just tore down, so a bare
    // "$ cmd" line was easy to lose) and, crucially, that Ctrl-C stops it.
    // The command still reads like a shell prompt so the transcript is
    // copy-paste-faithful.
    {
        std::string header;
        header += ui::dim();
        header += "\n╭─ running ─ Ctrl-C to stop ─────────────────────────────────";
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
    bool heartbeat_shown = false;
    int  spin = 0;
    static constexpr const char* kSpin[] = {"⣷","⣯","⣟","⡿","⣾","⣽","⣻","⣷"};
    for (;;) {
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 1000);
        if (pr == 0) {
            // Quiet second — draw/refresh the in-place heartbeat.
            if (ui::tty()) {
                const long secs = static_cast<long>(::time(nullptr) - started);
                std::string hb = "\r\x1b[2K";
                hb += ui::dim();
                hb += kSpin[spin++ % 8];
                hb += " running… " + std::to_string(secs) + "s (Ctrl-C to stop)";
                hb += ui::reset();
                ui::emit(hb);
                heartbeat_shown = true;
            }
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
        // Wipe any heartbeat line before real output lands on top of it.
        if (heartbeat_shown) { ui::clear_line(); heartbeat_shown = false; }
        (void)!::write(STDOUT_FILENO, buf, static_cast<std::size_t>(n));
        if (fin.output.size() < kCaptureMax) {
            const auto room = kCaptureMax - fin.output.size();
            fin.output.append(buf, std::min(static_cast<std::size_t>(n), room));
            if (static_cast<std::size_t>(n) > room) truncated = true;
        } else {
            truncated = true;
        }
    }
    if (heartbeat_shown) ui::clear_line();
    ::close(fds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::sigaction(SIGINT,  &old_int,  nullptr);
    ::sigaction(SIGQUIT, &old_quit, nullptr);

    if (WIFEXITED(status))        fin.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) fin.exit_code = 128 + WTERMSIG(status);
    else                          fin.exit_code = -1;
    if (truncated) fin.output += "\n[capture truncated at 2 MB — full output was shown on screen]";

    // Completion footer: a colour-coded status line so success/failure is
    // unmistakable at a glance, with elapsed time. The output stays in
    // native scrollback above the restored TUI either way.
    {
        const long secs = static_cast<long>(::time(nullptr) - started);
        const bool ok = (fin.exit_code == 0);
        std::string tail = "\n";
        tail += ok ? ui::green() : ui::red();
        tail += ok ? "╰─ ✓ done" : "╰─ ✗ failed";
        tail += ui::reset();
        tail += ui::dim();
        tail += "  exit " + std::to_string(fin.exit_code)
              + "  ·  " + std::to_string(secs) + "s"
              + "  ·  returning to agentty";
        tail += ui::reset();
        tail += "\n";
        ui::emit(tail);
    }
    return fin;
}

[[nodiscard]] maya::Cmd<Msg> run_block_cmd(std::string command, cbp::BlockShell /*shell*/) {
    return maya::Cmd<Msg>::suspend(
        [cmd = std::move(command)]() -> Msg {
            return Msg{run_on_real_tty(cmd)};
        });
}

#else  // _WIN32 — non-interactive fallback via the shared subprocess runner

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
    // UTF-16LE base64 for -EncodedCommand (PowerShell's contract).
    std::u16string u16;
    u16.reserve(body.size());
    for (unsigned char c : body) u16.push_back(static_cast<char16_t>(c));
    std::string bytes;
    bytes.reserve(u16.size() * 2);
    for (char16_t ch : u16) {
        bytes.push_back(static_cast<char>(ch & 0xFF));
        bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
    }
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    enc.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0u;
        const unsigned b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0u;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        enc.push_back(kB64[(triple >> 18) & 0x3F]);
        enc.push_back(kB64[(triple >> 12) & 0x3F]);
        enc.push_back(i + 1 < bytes.size() ? kB64[(triple >> 6) & 0x3F] : '=');
        enc.push_back(i + 2 < bytes.size() ? kB64[triple & 0x3F] : '=');
    }
    return "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand " + enc;
}

[[nodiscard]] maya::Cmd<Msg> run_block_cmd(std::string command, cbp::BlockShell shell) {
    return maya::Cmd<Msg>::task_isolated(
        [cmd = std::move(command), shell](std::function<void(Msg)> dispatch) {
            const std::string wrapped = wrap_for_windows_shell(shell, cmd);
            auto r = tools::util::run_command_s(wrapped);
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
            dispatch(Msg{std::move(fin)});
        });
}

#endif

} // namespace

Step codeblock_update(Model m, msg::CodeBlockMsg cm) {
    return std::visit(overload{
        [&](OpenCodeBlockPicker) -> Step {
            if (m.s.active()) {
                auto cmd = set_status_toast(m,
                    "wait for the reply to finish before grabbing blocks");
                return {std::move(m), std::move(cmd)};
            }
            auto blocks = latest_assistant_blocks(m);
            if (blocks.empty()) {
                auto cmd = set_status_toast(m,
                    "no code blocks in the last reply");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.code_blocks = cbp::Open{std::move(blocks), 0};
            m.ui.code_blocks_scroll.y = 0;
            return done(std::move(m));
        },
        [&](CloseCodeBlockPicker) -> Step {
            m.ui.code_blocks = cbp::Closed{};
            return done(std::move(m));
        },
        [&](CodeBlockPickerMove& e) -> Step {
            if (auto* o = code_block_picker_opened(m.ui.code_blocks)) {
                int sz = static_cast<int>(o->blocks.size());
                if (sz <= 0) return done(std::move(m));
                o->index = std::clamp(o->index + e.delta, 0, sz - 1);
                return done(std::move(m));
            }
            if (code_block_result(m.ui.code_blocks)) {
                // Read-only result card: Move deltas scroll the capture
                // viewport directly. max_y is paint-written-back by the
                // Picker widget; clamp against it (0 before first paint
                // — harmless, the writeback lands next frame).
                auto& sc = m.ui.code_blocks_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
            }
            return done(std::move(m));
        },
        [&](CodeBlockPickerSelect& e) -> Step {
            auto* o = code_block_picker_opened(m.ui.code_blocks);
            if (!o) return done(std::move(m));
            const int idx = (e.index >= 0) ? e.index : o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            CodeBlock block = o->blocks[static_cast<std::size_t>(idx)];
            const cbp::BlockShell shell = cbp::shell_for_language(block.language);
            if (shell == cbp::BlockShell::None) {
                // Not runnable on this platform — nudge toward the actions
                // that DO make sense for this block. Picker stays open so
                // `e` / `y` are one keystroke away.
                const std::string tag = block.language.empty()
                    ? std::string{"this"} : "'" + block.language + "'";
                auto cmd = set_status_toast(m,
                    tag + " block isn't runnable here — "
                    "press e to edit or y to copy");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.code_blocks = cbp::Closed{};
            return {std::move(m), run_block_cmd(std::move(block.body), shell)};
        },
        [&](CodeBlockPickerEdit) -> Step {
            auto* o = code_block_picker_opened(m.ui.code_blocks);
            if (!o) return done(std::move(m));
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.code_blocks = cbp::Closed{};
            // Splice at the cursor rather than replacing — same
            // convention as the @file / #symbol chips. The common case
            // is an empty composer, where this IS a replace.
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), body);
            m.ui.composer.cursor += static_cast<int>(body.size());
            return done(std::move(m));
        },
        [&](CodeBlockPickerCopy) -> Step {
            auto* o = code_block_picker_opened(m.ui.code_blocks);
            if (!o) return done(std::move(m));
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return done(std::move(m));
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.code_blocks = cbp::Closed{};
            auto toast = set_status_toast(m, "copied clean block to clipboard");
            return {std::move(m),
                    maya::Cmd<Msg>::batch(
                        maya::Cmd<Msg>::write_clipboard(std::move(body)),
                        std::move(toast))};
        },
        [&](CodeBlockRunFinished& e) -> Step {
            // Don't auto-stage — open the RESULT card instead. The user
            // already watched the output live on the real terminal; this
            // is the decision beat: attach the captured copy to the
            // composer, copy it clean, or discard. The composer only
            // ever receives output the user explicitly asked for.
            m.ui.code_blocks = cbp::Result{
                std::move(e.command), std::move(e.output),
                e.exit_code, e.timed_out};
            m.ui.code_blocks_scroll.y = 0;
            return done(std::move(m));
        },
        [&](CodeBlockResultAttach) -> Step {
            auto* r = code_block_result(m.ui.code_blocks);
            if (!r) return done(std::move(m));
            // Fold the captured output into the composer as an Output
            // attachment — the SAME collapse-to-chip / expand-on-submit
            // machinery a big paste uses. However huge the log is, the
            // composer shows one pill ("Output: sudo mkfs… · 1240 lines
            // · 48 KB"); the whole body only materialises on the wire
            // when the user actually submits.
            std::string out = std::move(r->output);
            std::size_t lines = out.empty() ? 0 : 1;
            for (char c : out) if (c == '\n') ++lines;

            Attachment att;
            att.kind       = Attachment::Kind::Output;
            att.name       = std::move(r->command);   // chip caption
            att.line_count = lines;
            att.byte_count = out.size();
            att.body       = std::move(out);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            m.ui.composer.expanded = true;
            m.ui.code_blocks = cbp::Closed{};
            auto toast = set_status_toast(m, "output attached to composer");
            return {std::move(m), std::move(toast)};
        },
        [&](CodeBlockResultCopy) -> Step {
            auto* r = code_block_result(m.ui.code_blocks);
            if (!r) return done(std::move(m));
            std::string body = std::move(r->output);
            m.ui.code_blocks = cbp::Closed{};
            auto toast = set_status_toast(m, "output copied to clipboard");
            return {std::move(m),
                    maya::Cmd<Msg>::batch(
                        maya::Cmd<Msg>::write_clipboard(std::move(body)),
                        std::move(toast))};
        },
        [&](CodeBlockResultDiscard) -> Step {
            m.ui.code_blocks = cbp::Closed{};
            return done(std::move(m));
        },
    }, cm);
}

} // namespace agentty::app::detail
