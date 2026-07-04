// reveal_scrollback_test — reproduce the streaming scrollback DUPLICATION
// with reveal_fx ON (the production view path).
//
// The scrollback_wire_fuzz harness deliberately forces reveal_fx OFF so
// frame heights are a pure function of model state. But the bug reported
// in production (the same prose line appearing twice in scrollback during
// a live stream) only manifests with reveal_fx ON — the live-edge overlay
// (scramble / gradient / caret) mutates the trailing rows of the canvas
// EVERY frame on its own wall clock, independent of whether new bytes
// arrived. If that per-frame mutation ever touches a row that has ALREADY
// overflowed the viewport top and committed to native scrollback, maya
// either rewrites a committed row (W4 violation) or its shrink/prefix
// reconciliation re-commits a shifted copy → the visible duplicate.
//
// This test drives the REAL ui::view (reveal_fx ON, exactly as turn.cpp
// builds it) through maya's REAL inline compose, streaming a long prose
// body that overflows a small viewport, then continues rendering frames
// (advancing wall-clock so the reveal overlay animates) and asserts:
//
//   R1  no wire row, once committed past the viewport top, is ever
//       rewritten by a later frame (the stranded-duplicate check).
//   R2  every render stays Synced (no Stale/HardReset full repaint).
//   R3  the prev_cells shadow verifies before every render.
//   R4  no UNIQUE prose line appears twice in a single rendered canvas
//       (the element-tree double-render check).
//   R5  THE SCREENSHOT SYMPTOM. A real ANSI terminal emulator (TermEmu)
//       consumes the actual BYTES maya writes to the fd, maintaining a
//       native scrollback + viewport. No unique prose line may appear
//       twice across the cumulative transcript (scrollback ++ screen) —
//       exactly what the user's eyes see. R1-R4 reconstruct rows from
//       maya's CANVAS and so cannot see content re-committed to native
//       scrollback by a from-the-top repaint; R5 can, and it catches
//       the production duplication: at the settle->freeze handoff the
//       reveal_fx widget's settled element tree diverges from the last
//       LIVE (reveal-animated) tree, so maya re-emits the whole turn,
//       landing a SECOND clean copy below the live (SGR-garbled) copy
//       already in scrollback.
//
// A failure means the reveal overlay (or the height/shape it implies)
// perturbs the committed scrollback prefix or forces a from-the-top
// re-emit — the production duplication bug.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <maya/render/canvas.hpp>
#include <maya/render/inline_frame.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
#include <maya/style/theme.hpp>
#include <maya/terminal/writer.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/tool/registry.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolUse;
using namespace maya;
using namespace maya::inline_frame;

static int g_failures = 0;
static int g_checks   = 0;

#define CHK(cond, detail)                                              \
    do { ++g_checks; if (!(cond)) {                                    \
        ++g_failures;                                                  \
        std::fprintf(stderr, "  FAIL: %s\n", std::string(detail).c_str()); \
    } } while (0)

// Paint the REAL view through render_tree under a sized context.
static Canvas paint_view(const Model& m, int width, int term_h,
                         StylePool& pool) {
    maya::RenderContext ctx{width, term_h, maya::render_generation(),
                            /*auto_height=*/true};
    maya::RenderContextGuard guard(ctx);
    Canvas c(width, 8000, &pool);
    c.clear();
    std::vector<layout::LayoutNode> nodes;
    maya::render_tree(agentty::ui::view(m), c, pool, maya::theme::dark, nodes,
                      /*auto_height=*/true);
    return c;
}

static std::vector<std::string> rows_of(const Canvas& c, int width) {
    std::vector<std::string> rows;
    const int mr = c.max_content_row();
    for (int y = 0; y <= mr; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            char32_t ch = c.get(x, y).character;
            line.push_back(ch && ch < 128 ? static_cast<char>(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        rows.push_back(std::move(line));
    }
    return rows;
}

static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    return {Writer{static_cast<platform::NativeHandle>(fds[1])}, fds[0]};
}

// Install a PTY pair as the process's STDOUT, sized to (cols x rows).
// CRITICAL: agentty's frozen.cpp sizes the trim's commit_scrollback(N)
// off term_dims(), which queries ioctl(TIOCGWINSZ) on STDOUT_FILENO. In
// a plain piped test that ioctl fails and falls back to 80x40 — geometry
// that disagrees with the harness's real (width x term_h) render, so the
// trim over-commits and strands a duplicate (a TEST artifact that masks
// whether the PRODUCTION freeze+trim path is actually correct). Backing
// stdout with a real PTY sized to the harness geometry makes term_dims()
// return the SAME size the renderer laid out at, so the trim's proof is
// geometry-consistent — exactly the production invariant. Returns the
// master fd (read side); writes maya sends to STDOUT come back here.
static int install_pty_stdout(int cols, int rows) {
    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::perror("openpty");
        return -1;
    }
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ioctl(slave, TIOCSWINSZ, &ws);
    // Redirect STDOUT to the slave so term_dims()'s ioctl on STDOUT_FILENO
    // sees (cols x rows). The slave is also the fd maya writes through.
    dup2(slave, STDOUT_FILENO);
    close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
    return master;
}

static void drain_fd(int rfd) {
    char buf[8192];
    while (read(rfd, buf, sizeof(buf)) > 0) {}
}

// Read ALL currently-available bytes from the pipe into a string (the
// frame maya just wrote). Non-blocking fd; we loop until EAGAIN.
static std::string read_fd(int rfd) {
    std::string s;
    char buf[8192];
    for (;;) {
        ssize_t n = read(rfd, buf, sizeof(buf));
        if (n > 0) { s.append(buf, static_cast<std::size_t>(n)); continue; }
        break;
    }
    return s;
}

// ───────────────────────────────────────────────────────────────────────
// A real ANSI terminal emulator with native scrollback.
//
// The canvas-reconstruction harness (Harness::frame) can only see what
// maya's CANVAS describes for a single frame — it cannot see the actual
// BYTES maya writes to the fd, nor what the terminal's NATIVE SCROLLBACK
// accumulates as rows scroll off the top. The production duplication bug
// ("Let me check the diagram source:" shown TWICE at once) lives in the
// cumulative terminal state: a row commits to scrollback, then a later
// frame's cursor-up/EL/scroll dance re-paints the same content again
// inside the viewport — so the screen now shows it in BOTH places.
//
// This emulator consumes maya's exact escape vocabulary (cursor up/down/
// forward, EL \x1b[K, ED \x1b[J, \r, \n with bottom-edge scroll into
// scrollback, DECAWM ?7l, SGR, sync ?2026h/l, hide/show cursor, the
// HardReset wipe \x1b[2J\x1b[3J\x1b[H) and maintains:
//   - a viewport of `rows` lines, each `cols` wide
//   - a native scrollback vector (rows that scrolled off the top)
// so the test can run duplication checks over scrollback + viewport
// TOGETHER, exactly what the user's eyes see.
struct TermEmu {
    int cols, rows;
    int cx = 0, cy = 0;                  // cursor (viewport-relative)
    std::vector<std::string> screen;    // rows lines, each cols chars
    std::vector<std::string> scrollback;// committed rows (immutable history)
    bool decawm = true;                 // auto-wrap on by default

    TermEmu(int c, int r) : cols(c), rows(r) {
        screen.assign(static_cast<std::size_t>(rows),
                      std::string(static_cast<std::size_t>(cols), ' '));
    }

    std::string& line(int y) { return screen[static_cast<std::size_t>(y)]; }

    void put(char ch) {
        if (cx >= cols) {
            if (decawm) { cx = 0; cursor_newline(); }
            else        { cx = cols - 1; }   // clamp at right edge
        }
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols)
            l.resize(static_cast<std::size_t>(cols), ' ');
        l[static_cast<std::size_t>(cx)] = ch;
        ++cx;
    }

    // \n at the bottom row scrolls the viewport up; the row leaving the
    // top is committed to native scrollback (exactly a real terminal).
    void cursor_newline() {
        if (cy < rows - 1) { ++cy; return; }
        // scroll up by one
        std::string top = screen.front();
        while (!top.empty() && top.back() == ' ') top.pop_back();
        scrollback.push_back(std::move(top));
        screen.erase(screen.begin());
        screen.push_back(std::string(static_cast<std::size_t>(cols), ' '));
        // cy stays at rows-1
    }

    void carriage_return() { cx = 0; }
    void cursor_up(int n)   { cy -= n; if (cy < 0) cy = 0; }
    void cursor_down(int n) { for (int i = 0; i < n; ++i) cursor_newline(); }
    void cursor_fwd(int n)  { cx += n; if (cx > cols) cx = cols; }

    void erase_to_eol() {  // \x1b[K
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols)
            l.resize(static_cast<std::size_t>(cols), ' ');
        for (int x = cx; x < cols; ++x) l[static_cast<std::size_t>(x)] = ' ';
    }

    void erase_to_eos() {  // \x1b[J — erase cursor->EOL on this row, then
        erase_to_eol();    // every row below it.
        for (int y = cy + 1; y < rows; ++y)
            line(y).assign(static_cast<std::size_t>(cols), ' ');
    }

    void full_wipe() {  // \x1b[2J\x1b[3J\x1b[H — clear screen + scrollback
        for (auto& l : screen) l.assign(static_cast<std::size_t>(cols), ' ');
        scrollback.clear();
        cx = cy = 0;
    }

    // Feed a chunk of maya's wire bytes.
    void feed(const std::string& b) {
        std::size_t i = 0, n = b.size();
        while (i < n) {
            unsigned char ch = static_cast<unsigned char>(b[i]);
            if (ch == '\r') { carriage_return(); ++i; continue; }
            if (ch == '\n') { carriage_return_optional_then_lf(); ++i; continue; }
            if (ch == 0x1b) { i = parse_esc(b, i); continue; }
            // printable (treat any >= 0x20 byte as a glyph; multibyte
            // UTF-8 lead/continuation bytes each occupy a cell slot in
            // our coarse model — fine for ASCII-only test prose).
            if (ch >= 0x20) { put(static_cast<char>(ch < 128 ? ch : '?')); ++i; continue; }
            ++i; // ignore other C0
        }
    }

    // Plain \n in maya output advances the line (it precedes content with
    // \r where needed). Maya emits \r\n between rows; the \r already reset
    // cx. A bare \n during the scroll loop must scroll at the bottom.
    void carriage_return_optional_then_lf() { cursor_newline(); }

    // Parse one CSI/escape sequence starting at b[i] (b[i] == ESC).
    // Returns the index just past the sequence.
    std::size_t parse_esc(const std::string& b, std::size_t i) {
        const std::size_t n = b.size();
        if (i + 1 >= n) return n;
        if (b[i + 1] != '[') return i + 2;   // non-CSI esc: skip 2
        std::size_t j = i + 2;
        bool priv = false;
        if (j < n && b[j] == '?') { priv = true; ++j; }
        int param = 0; bool have_param = false;
        while (j < n && b[j] >= '0' && b[j] <= '9') {
            param = param * 10 + (b[j] - '0'); have_param = true; ++j;
        }
        if (j >= n) return n;
        char final = b[j];
        const int p = have_param ? param : 0;
        switch (final) {
            case 'A': cursor_up(p ? p : 1); break;
            case 'B': cursor_down(p ? p : 1); break;
            case 'C': cursor_fwd(p ? p : 1); break;
            case 'D': cx -= (p ? p : 1); if (cx < 0) cx = 0; break;
            case 'G': cx = (p ? p - 1 : 0); if (cx < 0) cx = 0; break;
            case 'H': cy = 0; cx = 0; break;       // CUP (only home used here)
            case 'K': erase_to_eol(); break;       // EL 0
            case 'J':
                if (p == 2 || p == 3) {            // ED 2/3 — clear screen
                    if (p == 3) scrollback.clear();
                    for (auto& l : screen)
                        l.assign(static_cast<std::size_t>(cols), ' ');
                } else {
                    erase_to_eos();                // ED 0
                }
                break;
            case 'h': if (priv && p == 7) decawm = true; break;  // DECAWM on
            case 'l': if (priv && p == 7) decawm = false; break; // DECAWM off
            case 'm': break;                        // SGR — ignore styling
            default: break;                         // ?25h/l, ?2026h/l, etc.
        }
        return j + 1;
    }

    // The full visible-to-the-user transcript: scrollback then screen.
    std::vector<std::string> transcript() const {
        std::vector<std::string> t = scrollback;
        for (const auto& l : screen) {
            std::string s = l;
            while (!s.empty() && s.back() == ' ') s.pop_back();
            t.push_back(std::move(s));
        }
        return t;
    }
};

// Wire harness: carry maya's Synced frame across frames, model the
// committed (immutable) scrollback rows, and check R1/R2/R3 every frame.
struct Harness {
    int width, term_h;
    StylePool pool;
    Writer writer;
    int rfd;
    std::optional<InlineFrame<Synced>> synced;
    std::vector<std::string> wire;   // committed rows (immutable forever)
    std::size_t commits = 0;
    std::size_t last_wrote = 0;      // bytes maya wrote on the last frame
    bool dead = false;
    TermEmu emu;                     // real terminal: scrollback + viewport

    Harness(int w, int th)
        : width(w), term_h(th), emu(w, th) {
        auto pw = make_pipe_writer();
        writer = std::move(pw.first);
        rfd = pw.second;
    }
    ~Harness() { close(rfd); }

    // R5: run the duplication check over the FULL cumulative terminal
    // transcript (native scrollback + current viewport) — exactly what
    // the user sees. A unique test-prose line appearing at two distinct
    // absolute rows of the live screen+scrollback is the screenshot bug:
    // the same body painted in two places at once. We only flag lines
    // that are simultaneously present in the on-screen VIEWPORT region
    // (the visible duplicate the user reported) — a line legitimately in
    // deep scrollback AND re-shown live is the corruption.
    void check_emu(const std::string& tag) {
        auto scr = emu.scrollback;
        // viewport (trailing) rows, blank-trimmed:
        std::vector<std::string> view;
        for (const auto& l : emu.screen) {
            std::string s = l;
            while (!s.empty() && s.back() == ' ') s.pop_back();
            view.push_back(std::move(s));
        }
        auto is_prose = [](const std::string& ln) {
            return ln.find("tracing call path-")     != std::string::npos
                || ln.find("serializes runs batch-") != std::string::npos
                || ln.find("diffs canvas-")          != std::string::npos
                || ln.find("builds tree-")           != std::string::npos;
        };
        // Full transcript = scrollback ++ viewport. Index space is
        // absolute (scrollback first). A unique prose line must appear
        // at most once across the WHOLE transcript.
        std::vector<std::string> all = scr;
        for (auto& v : view) all.push_back(v);
        std::vector<std::pair<std::string,int>> seen;
        for (int y = 0; y < static_cast<int>(all.size()); ++y) {
            const std::string& ln = all[static_cast<std::size_t>(y)];
            if (!is_prose(ln)) continue;
            for (auto& [s, py] : seen) {
                if (s == ln) {
                    CHK(false,
                        "R5: line duplicated in terminal transcript @" + tag
                        + " abs-rows " + std::to_string(py) + " and "
                        + std::to_string(y)
                        + " (scrollback=" + std::to_string(scr.size())
                        + ")\n      |" + ln + "|");
                    std::fprintf(stderr,
                        "    --- terminal transcript (%zu sb + %zu view) ---\n",
                        scr.size(), view.size());
                    for (int yy = 0; yy < static_cast<int>(all.size()); ++yy)
                        std::fprintf(stderr, "    %3d|%s%s\n", yy,
                            yy < static_cast<int>(scr.size()) ? "" : "V ",
                            all[static_cast<std::size_t>(yy)].c_str());
                    dead = true;
                    return;
                }
            }
            seen.emplace_back(ln, y);
        }

        // ── R6: THE COMPOSER-DUPLICATION SYMPTOM ────────────────────
        // After a turn settles, the reported bug is a SECOND copy of the
        // composer input box stranded one screen up (in native
        // scrollback), while the live composer rides the viewport bottom.
        // The composer's idle placeholder row renders "❯ type a
        // message…"; the non-ASCII prompt chip degrades to spaces in the
        // coarse TermEmu but the ASCII substring "type a message" is
        // stable. It must appear AT MOST ONCE across the whole transcript
        // (scrollback ++ viewport). Two occurrences = the stranded
        // duplicate the user sees.
        auto is_composer = [](const std::string& ln) {
            return ln.find("type a message")      != std::string::npos
                || ln.find("streaming \xe2\x80\x94 type to queue")
                       != std::string::npos;
        };
        int composer_hits = 0;
        int first_composer = -1;
        for (int y = 0; y < static_cast<int>(all.size()); ++y) {
            if (!is_composer(all[static_cast<std::size_t>(y)])) continue;
            ++composer_hits;
            if (first_composer < 0) first_composer = y;
            else {
                CHK(false,
                    "R6: composer duplicated in terminal transcript @" + tag
                    + " abs-rows " + std::to_string(first_composer) + " and "
                    + std::to_string(y)
                    + " (scrollback=" + std::to_string(scr.size()) + ")");
                std::fprintf(stderr,
                    "    --- terminal transcript (%zu sb + %zu view) ---\n",
                    scr.size(), view.size());
                for (int yy = 0; yy < static_cast<int>(all.size()); ++yy)
                    std::fprintf(stderr, "    %3d|%s%s\n", yy,
                        yy < static_cast<int>(scr.size()) ? "" : "V ",
                        all[static_cast<std::size_t>(yy)].c_str());
                dead = true;
                return;
            }
        }
    }

    // Apply a trim's commit_scrollback(N) Cmd to maya's Synced state
    // BEFORE the next render — exactly what the runtime does. meta.cpp /
    // modal.cpp forward the trim Cmd and the driver turns it into
    // synced.commit(scrollback_marker(min(N, prev_rows-term_h))) against
    // the PRIOR frame's still-valid Synced state (commit_inline_overflow
    // semantics). Without this the harness diverges from production and
    // the freeze frame re-emits the whole turn — but that divergence is
    // ALSO the real bug surface, so we keep both code paths exercised.
    void apply_trim(maya::Cmd<agentty::Msg>& cmd) {
        if (dead || !synced) return;
        using Cmd = maya::Cmd<agentty::Msg>;
        const auto* c = std::get_if<Cmd::CommitScrollback>(&cmd.inner);
        if (!c) return;
        const int prev_rows = synced->rows();
        const int safe = std::min(c->rows, std::max(0, prev_rows - term_h));
        if (safe <= 0) return;
        synced = std::move(*synced).commit(synced->scrollback_marker(safe));
        commits += static_cast<std::size_t>(safe);
    }

    void frame(const Model& m, const std::string& tag) {
        if (dead) return;
        Canvas c = paint_view(m, width, term_h, pool);
        auto cur = rows_of(c, width);
        const int R = static_cast<int>(cur.size());
        if (R <= 0) return;

        // R4: WITHIN-FRAME duplicate detection. The same distinctive prose
        // line must not appear at two different rows of a SINGLE rendered
        // canvas. This is the screenshot symptom directly: "Let me check
        // the diagram source:" visible twice at once means the element
        // tree itself carries the text twice (frozen prefix AND live tail,
        // or two live sub-turns) — a view-layer bug the wire invariants
        // (R1-R3) can't see because each frame is internally consistent.
        {
            std::vector<std::pair<std::string,int>> seen;
            for (int y = 0; y < R; ++y) {
                const std::string& ln = cur[static_cast<std::size_t>(y)];
                // Only check the test's UNIQUE prose body lines. Every
                // paragraph/step the scenarios stream carries a unique
                // integer tag ("tracing call path-N", "diffs canvas-N"),
                // so a TRUE within-frame duplicate of one of these means
                // the same message body got rendered twice (frozen prefix
                // AND live tail, or a sub-turn rendered in two places) —
                // the exact screenshot bug. Tool-card chrome ("Read 8
                // lines", "ACTIONS") legitimately repeats per panel and is
                // intentionally excluded.
                const bool is_test_prose =
                    ln.find("tracing call path-") != std::string::npos
                 || ln.find("serializes runs batch-") != std::string::npos
                 || ln.find("diffs canvas-") != std::string::npos
                 || ln.find("builds tree-") != std::string::npos;
                if (!is_test_prose) continue;
                bool is_dup = false;
                for (auto& [s, py] : seen) {
                    if (s == ln) {
                        CHK(false,
                            "R4: line duplicated within one frame @" + tag
                            + " rows " + std::to_string(py) + " and "
                            + std::to_string(y) + "\n      |" + ln + "|");
                        std::fprintf(stderr, "    --- full frame (%d rows) ---\n", R);
                        for (int yy = 0; yy < R; ++yy)
                            std::fprintf(stderr, "    %3d|%s\n", yy,
                                         cur[static_cast<std::size_t>(yy)].c_str());
                        is_dup = true;
                        dead = true;
                        break;
                    }
                }
                if (is_dup) return;
                seen.emplace_back(ln, y);
            }
        }

        if (!synced) {
            auto o = InlineFrame<Empty>{}.seed().render(
                c, content_rows(c), term_rows_for_test(term_h), pool,
                writer, false);
            emu.feed(read_fd(rfd));
            synced = std::visit(
                [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                        return std::move(a);
                    else return std::nullopt;
                }, std::move(o));
            CHK(synced.has_value(), "R2: first frame not Synced @" + tag);
            if (!synced) { dead = true; return; }
        } else {
            // Mirror maya's app.cpp SINGLE scrollback-invariant gate:
            // if the frame is OVERFLOWED and the committed overflow
            // prefix no longer matches the wire, the per-row diff is
            // unsafe (it would rewrite immutable native scrollback).
            // Recover BEFORE the diff — exactly one memcmp, uniform for
            // shrink / grow / any future cause:
            //   • growing (R > prev_rows): case (B) would re-overflow, so
            //     HardReset (\x1b[2J\x1b[3J\x1b[H wipe + fresh paint);
            //   • not growing: commit off-viewport rows + soft-repaint.
            const int prev_rows = synced->rows();
            if (prev_rows > term_h
                && !synced->scrollback_prefix_matches(c, prev_rows - term_h)) {
                const int overflow = prev_rows - term_h;
                if (R > prev_rows) {
                    // Growing + prefix shift → HardReset.
                    auto hr = std::move(*synced).demote_to_hard_reset();
                    auto o = std::move(hr).render(
                        c, content_rows(c), term_rows_for_test(term_h), pool,
                        writer, false);
                    { std::string b = read_fd(rfd); last_wrote = b.size(); emu.feed(b); }
                    // HardReset wipes native scrollback (\x1b[3J) and
                    // repaints from the top: the wire shadow is now
                    // stale, so reset it to match the fresh paint.
                    wire.clear();
                    commits = 0;
                    synced = std::visit(
                        [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                            using T = std::decay_t<decltype(a)>;
                            if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                                return std::move(a);
                            else return std::nullopt;
                        }, std::move(o));
                    CHK(synced.has_value(),
                        "R3: hard-reset recovery did not return Synced @" + tag);
                    if (!synced) { dead = true; return; }
                    if (std::getenv("REVEAL_DBG"))
                        std::fprintf(stderr, "[dbg] %-14s HARD-RESET grow\n",
                                     tag.c_str());
                    check_emu(tag);
                    return;
                }
                // Not growing + prefix shift → commit + soft-repaint.
                synced = std::move(*synced).commit(
                    synced->scrollback_marker(overflow));
                commits += static_cast<std::size_t>(overflow);
            }
            auto wit = synced->verify();
            if (!wit) {
                // REAL-RUNTIME RECOVERY (app.cpp): a poisoned shadow does
                // NOT kill the session — it demotes to Stale and the next
                // render goes through compose case (B). When the frame has
                // overflowed the viewport, app.cpp first commits the
                // off-viewport rows, then demotes. Model that here so the
                // harness exercises the SAME path production takes at a
                // settle whose shadow diverged — the path that stranded the
                // composer. (Previously the harness just died on R3, hiding
                // the case-(B) over-serialize entirely.)
                const int prev_rows = synced->rows();
                if (prev_rows > term_h) {
                    const int overflow = prev_rows - term_h;
                    synced = std::move(*synced).commit(
                        synced->scrollback_marker(overflow));
                    commits += static_cast<std::size_t>(overflow);
                }
                auto stale = std::move(*synced).demote_to_stale();
                auto o = std::move(stale).render(
                    c, content_rows(c), term_rows_for_test(term_h), pool,
                    writer, false);
                { std::string b = read_fd(rfd); last_wrote = b.size(); emu.feed(b); }
                synced = std::visit(
                    [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                        using T = std::decay_t<decltype(a)>;
                        if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                            return std::move(a);
                        else return std::nullopt;
                    }, std::move(o));
                CHK(synced.has_value(),
                    "R3: stale recovery did not return Synced @" + tag);
                if (!synced) { dead = true; return; }
            } else {
                auto o = std::move(*synced).render(
                    c, content_rows(c), term_rows_for_test(term_h), pool,
                    writer, std::move(*wit), false);
                { std::string b = read_fd(rfd); last_wrote = b.size(); emu.feed(b); }
                synced = std::visit(
                    [](auto&& a) -> std::optional<InlineFrame<Synced>> {
                        using T = std::decay_t<decltype(a)>;
                        if constexpr (std::is_same_v<T, InlineFrame<Synced>>)
                            return std::move(a);
                        else return std::nullopt;
                    }, std::move(o));
                CHK(synced.has_value(), "R2: render demoted out of Synced @" + tag);
                if (!synced) { dead = true; return; }
            }
        }

        // R1: wire-model byte check. Canvas row y → absolute wire index
        // commits + y. Rows past the viewport top (R - term_h) are
        // committed from now on; any already-captured index must match.
        //
        // CAVEAT: the reveal_fx overlay transiently INFLATES canvas height
        // (ghost-extend + scramble tail) on the live edge without those
        // rows ever physically scrolling off in the real terminal. So the
        // canvas-derived `R - term_h` boundary can momentarily mark an
        // IN-VIEWPORT row as "committed" and then the row legitimately
        // mutates next frame (the live line growing). That is NOT
        // corruption — nothing scrolled off. The authoritative immutable-
        // row set is the EMULATOR's native scrollback (built from the real
        // bytes). Only treat a wire index as committed-immutable once the
        // emulator has actually scrolled it off (wi < emu.scrollback.size()).
        // R5 below is the on-screen-duplication ground truth; R1 is the
        // tighter wire-immutability guard, scoped here to genuinely-
        // scrolled-off rows so reveal height jitter can't false-positive.
        const int over = R > term_h ? R - term_h : 0;
        const std::size_t real_sb = emu.scrollback.size();
        for (int y = 0; y < over; ++y) {
            const std::size_t wi = commits + static_cast<std::size_t>(y);
            if (wi < wire.size()) {
                // Only assert immutability for rows the REAL terminal has
                // committed to scrollback; in-viewport rows (even if past
                // the inflated canvas boundary) may still change.
                if (wi < real_sb && wire[wi] != cur[static_cast<std::size_t>(y)]) {
                    CHK(false,
                        "R1: committed wire row " + std::to_string(wi)
                        + " REWRITTEN @" + tag
                        + "\n      was |" + wire[wi]
                        + "|\n      now |" + cur[static_cast<std::size_t>(y)] + "|");
                    dead = true;
                    return;
                }
                // Keep the shadow tracking the latest in-viewport content
                // so a later genuine scroll-off compares against the right
                // bytes.
                if (wi >= real_sb) wire[wi] = cur[static_cast<std::size_t>(y)];
            } else if (wi == wire.size()) {
                wire.push_back(cur[static_cast<std::size_t>(y)]);
            } else {
                wire.resize(wi, std::string{});
                wire.push_back(cur[static_cast<std::size_t>(y)]);
            }
        }

        // R5: the screenshot symptom — run duplication detection over the
        // REAL terminal transcript (native scrollback + viewport) built
        // from the actual bytes maya wrote, not the canvas reconstruction.
        if (std::getenv("REVEAL_DBG")) {
            std::fprintf(stderr,
                "[dbg] %-14s R=%d prev=%d commits=%zu sb=%zu wrote=%zu\n",
                tag.c_str(), R,
                synced ? synced->rows() : -1, commits,
                emu.scrollback.size(), last_wrote);
        }
        check_emu(tag);
    }
};

// Sleep a beat so the reveal overlay's wall clock advances between
// frames — that's what makes the animated trailing edge mutate even on a
// frame where no new bytes arrived (the production RAF cadence).
static void tick() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// Drive the production streaming cadence with reveal_fx ON (the real
// ui::view builds the StreamingMarkdown with set_reveal_fx(true), so we
// just feed the model and render — the view installs the widget).
static void run_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"reveal"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "welcome");

    // 1) A settled user turn, frozen — gives the stream a committed
    //    prefix above it so subsequent overflow has somewhere to land.
    {
        Message u; u.role = Role::User;
        u.text = "Please walk through the whole rendering pipeline in detail.";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        h.frame(m, "submit");
    }

    // 2) Start streaming an assistant reply.
    Message a; a.role = Role::Assistant;
    a.streaming_text = "Let me check the diagram source:";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Render the opening several times WITHOUT new bytes — this is the
    // critical window: the reveal overlay animates the live edge on its
    // own clock while the byte content is frozen. If the overlay or its
    // implied height perturbs an already-committed row, R1 fires.
    for (int f = 0; f < 6; ++f) { tick(); h.frame(m, "open" + std::to_string(f)); }

    // 3) Stream a long body that overflows the small viewport so the
    //    opening line ("Let me check the diagram source:") scrolls past
    //    the top and commits to scrollback. Keep rendering with reveal
    //    animating between every delta.
    auto& back = m.d.current.messages.back();
    for (int d = 0; d < 60; ++d) {
        back.streaming_text +=
            "\n\nParagraph " + std::to_string(d)
            + " (#" + std::to_string(d * 7 + 3) + "): the renderer stage "
            + std::to_string(d) + " builds tree-" + std::to_string(d)
            + ", lays out region-" + std::to_string(d * 2)
            + ", diffs canvas-" + std::to_string(d * 3)
            + " and serializes runs batch-" + std::to_string(d * 5) + ".";
        tick();
        h.frame(m, "delta" + std::to_string(d));
        // An extra no-byte animation frame between deltas — pure reveal
        // motion over a committed prefix (the production RAF tick).
        tick();
        h.frame(m, "anim" + std::to_string(d));
        if (h.dead) return;
    }

    // 4) Settle: move streaming_text → text, settle the md cache, idle,
    //    then WAIT for the reveal to fully drain before freezing —
    //    exactly the production gate (meta.cpp Tick freezes only once
    //    live_tail_reveal_settled(m) is true). Freezing before the
    //    reveal drains captures a tree that diverges from the last live
    //    (reveal-animated) frame → maya re-emits the whole turn.
    {
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        agentty::app::detail::settle_message_md(m, b);
        m.s.phase = agentty::phase::Idle{};
        // Deferred-settle paints: tick + render until the reveal has
        // fully drained (bounded so a hang can't wedge the test).
        int guard = 0;
        do {
            tick();
            h.frame(m, "settle" + std::to_string(guard));
        } while (!agentty::app::detail::live_tail_reveal_settled(m)
                 && ++guard < 200 && !h.dead);
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        // Trim runs on geometry-consistent dims now (PTY-backed stdout),
        // exactly as production: the commit_scrollback(N) it returns is
        // sized off the SAME term_dims() the renderer laid out at.
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "freeze");
    }
    if (pty_master >= 0) close(pty_master);
}

// Multi-sub-turn scenario — the screenshot shape: text → ACTIONS panel →
// continuation text → ACTIONS panel … all inside ONE live (un-frozen)
// assistant run. When a tool sub-turn lands, the prior text message's
// streaming_text is settled into `.text` (production: settle_message_md)
// but the message STAYS in the live tail because the run isn't terminal
// (a continuation is pending). A fresh StreamingMarkdown widget is built
// for the continuation. Reveal_fx is ON for every widget. The whole run
// grows past the viewport so early sub-turns overflow + commit. If any
// settled-but-still-live earlier sub-turn re-lays-out or its reveal
// overlay perturbs a committed row, R1 fires — the visible duplicate.
static ToolUse done_tool(const std::string& tag) {
    ToolUse t;
    auto now = std::chrono::steady_clock::now();
    t.id = agentty::ToolCallId{"read_" + tag};
    t.name = agentty::ToolName{"read"};
    std::string out;
    for (int i = 0; i < 8; ++i)
        out += std::to_string(i) + ": " + tag + " source line\n";
    t.status = ToolUse::Done{now - std::chrono::milliseconds{5}, now,
                             std::move(out)};
    return t;
}

// A tool still RUNNING: its card renders the spinner/awaiting body (~1
// row) — SHORT. When it later transitions terminal it SWAPS to the full
// output/error body, GROWING the card. That mid-turn height grow, over a
// live tail that has already overflowed the viewport, is the exact seam
// the screenshot corruption lives at.
static ToolUse running_tool(const std::string& tag) {
    ToolUse t;
    auto now = std::chrono::steady_clock::now();
    t.id   = agentty::ToolCallId{"bash_" + tag};
    t.name = agentty::ToolName{"bash"};
    t.args = nlohmann::json{{"command", "git commit -m " + tag}};
    t.status = ToolUse::Running{now - std::chrono::milliseconds{5}};
    return t;
}

// Turn a running tool into a FAILED one carrying a multi-line error body
// is driven directly through the real apply_tool_output reducer in the
// scenario below (so the production height-grow + cooldown-arm path is
// exercised), not via a hand-built status here.

// A settled WRITE tool card with a LONG body. Once terminal, the view
// renders it show_all=true (full body) in BOTH the live tail and the
// frozen snapshot — a TALL colored card. This is the shape that sat in
// the user's scrollback (a prior `write random.txt` diff) while a NEW
// turn settled above it and stranded a duplicate composer. `lines`
// controls the body height so the card overflows the viewport.
static ToolUse settled_write_tool(const std::string& tag, int lines) {
    ToolUse t;
    auto now = std::chrono::steady_clock::now();
    t.id   = agentty::ToolCallId{"write_" + tag};
    t.name = agentty::ToolName{"write"};
    std::string content;
    for (int i = 0; i < lines; ++i)
        content += "line " + std::to_string(i) + ": " + tag
                 + " written body content that wraps a bit\n";
    t.args = nlohmann::json{{"path", "random_" + tag + ".txt"},
                            {"content", content}};
    std::string out = "wrote " + std::to_string(lines) + " lines to random_"
                    + tag + ".txt";
    t.status = ToolUse::Done{now - std::chrono::milliseconds{5}, now,
                             std::move(out)};
    return t;
}

// PRIOR-WRITE-CARD scenario — the exact shape of the reported bug.
// A completed turn containing a TALL settled write card is frozen into
// the prefix (it lives in native scrollback, like the user's earlier
// `write random.txt`). THEN a second turn streams a reply that overflows
// and settles. On the settle frame the canvas contains the (full-body)
// write card in the frozen prefix; if maya re-serializes that committed
// region, the composer band is pushed a second time into scrollback —
// R6 fires. Runs on TALL terminals (the reported geometry).
static void run_prior_writecard_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealpw"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);

    // Turn 1: user asks, assistant writes a tall file. Build the model
    // fully FIRST, then render the initial frame already at the settled
    // height — this mirrors a resumed session (rehydrate) rather than a
    // welcome→content jump, so the first frame's grow is not conflated
    // with the settle grow we're actually testing.
    {
        Message u; u.role = Role::User;
        u.text = "Create random.txt with some nonsense.";
        m.d.current.messages.push_back(std::move(u));

        Message a; a.role = Role::Assistant;
        a.text = "Done \xe2\x80\x94 created random.txt with a bit of nonsense.";
        a.tool_calls.push_back(settled_write_tool("r1", term_h + 20));
        m.d.current.messages.push_back(std::move(a));

        agentty::app::detail::settle_message_md(
            m, m.d.current.messages.back());
        agentty::app::detail::rehydrate_frozen(m);
        h.frame(m, "pw-turn1-rehydrated");
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "pw-turn1-trim");
    }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }

    // Turn 2: user asks again; assistant streams a reply that overflows,
    // then settles. This is the settle that stranded the composer.
    {
        Message u; u.role = Role::User;
        u.text = "now edit it";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        h.frame(m, "pw-turn2-submit");
    }

    Message a2; a2.role = Role::Assistant;
    a2.streaming_text = "Sure, here are three trivia facts, unrelated:";
    m.d.current.messages.push_back(std::move(a2));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    for (int f = 0; f < 4; ++f) { tick(); h.frame(m, "pw-open" + std::to_string(f)); }

    auto& back = m.d.current.messages.back();
    for (int d = 0; d < 40 && !h.dead; ++d) {
        back.streaming_text +=
            "\n\nParagraph " + std::to_string(d)
            + ": builds tree-" + std::to_string(d)
            + ", diffs canvas-" + std::to_string(d)
            + ", serializes runs batch-" + std::to_string(d) + ".";
        tick(); h.frame(m, "pw-d" + std::to_string(d));
        tick(); h.frame(m, "pw-anim" + std::to_string(d));
    }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }

    // Settle turn 2 through the production gate.
    {
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        agentty::app::detail::settle_message_md(m, b);
        m.s.phase = agentty::phase::Idle{};
        int guard = 0;
        do {
            tick();
            h.frame(m, "pw-settle" + std::to_string(guard));
        } while (!agentty::app::detail::live_tail_reveal_settled(m)
                 && ++guard < 200 && !h.dead);
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "pw-freeze");
        // A couple of idle animation frames after the freeze (the
        // settle_cooldown window) — the composer keeps repainting; any
        // stranded copy is now visible to R6.
        for (int f = 0; f < 4 && !h.dead; ++f) { tick(); h.frame(m, "pw-idle" + std::to_string(f)); }
    }
    if (pty_master >= 0) close(pty_master);
}

static void run_multiturn_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealmt"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "mt-welcome");

    // Frozen user turn so the live run has a committed prefix above it.
    {
        Message u; u.role = Role::User;
        u.text = "Investigate the rendering pipeline end to end.";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        h.frame(m, "mt-submit");
    }

    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Several text→tool→continuation sub-turns inside ONE live run.
    for (int turn = 0; turn < 6 && !h.dead; ++turn) {
        const std::string st = std::to_string(turn);

        // New assistant sub-turn message: stream prose into it.
        Message a; a.role = Role::Assistant;
        a.streaming_text = "Let me check diagram source " + st + ":";
        m.d.current.messages.push_back(std::move(a));
        for (int f = 0; f < 4; ++f) { tick(); h.frame(m, "mt" + st + "-open" + std::to_string(f)); }

        // Grow the prose a bit (overflow accumulates across sub-turns).
        auto& back = m.d.current.messages.back();
        for (int d = 0; d < 4; ++d) {
            back.streaming_text +=
                "\n\nStep " + st + "." + std::to_string(d)
                + " (read-" + std::to_string(turn * 10 + d)
                + "): tracing call path-" + std::to_string(turn * 4 + d)
                + " through layout-" + std::to_string(turn)
                + " and serialize-" + std::to_string(d) + ".";
            tick(); h.frame(m, "mt" + st + "-d" + std::to_string(d));
        }

        // Tool sub-turn lands: production settles the prior text into
        // `.text` (it stays in the live tail) and pushes the tool message.
        {
            auto& b = m.d.current.messages.back();
            b.text += b.streaming_text;
            b.streaming_text.clear();
            agentty::app::detail::settle_message_md(m, b);
        }
        Message tmsg; tmsg.role = Role::Assistant;
        tmsg.tool_calls.push_back(done_tool("mt" + st));
        m.d.current.messages.push_back(std::move(tmsg));
        for (int f = 0; f < 3; ++f) { tick(); h.frame(m, "mt" + st + "-tool" + std::to_string(f)); }
    }

    // Final settle + freeze.
    {
        auto& b = m.d.current.messages.back();
        if (!b.streaming_text.empty()) {
            b.text += b.streaming_text;
            b.streaming_text.clear();
            agentty::app::detail::settle_message_md(m, b);
        }
        m.s.phase = agentty::phase::Idle{};
        for (int f = 0; f < 12; ++f) { tick(); h.frame(m, "mt-settle" + std::to_string(f)); }
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "mt-freeze");
    }
    if (pty_master >= 0) close(pty_master);
}

// Submit-mid-reveal scenario — mirrors modal.cpp's submit path, which
// force-freezes the prior assistant turn (settle_message_md + freeze_
// through) the instant the user hits Enter, EVEN IF the reveal is still
// mid-glide (pending_settle_freeze). If the frozen snapshot captures a
// live-overlay tree that diverges from what the next frame paints, the
// prior turn's body can land in BOTH the frozen prefix and a re-render —
// the duplicate. We stream a body that overflows, then submit WITHOUT any
// reveal-drain wait, and render. R1/R4 catch any duplication.
static void run_submit_mid_reveal_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealsub"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "sub-welcome");

    for (int turn = 0; turn < 4 && !h.dead; ++turn) {
        const std::string st = std::to_string(turn);

        // User submits (first turn: just push; later turns: this is the
        // mid-reveal submit that force-freezes the prior assistant turn).
        Message u; u.role = Role::User;
        u.text = "Question " + st + ": explain a subsystem.";
        m.d.current.messages.push_back(std::move(u));
        // Mirror modal.cpp submit: settle prior assistant turns then
        // freeze through, WITHOUT waiting for reveal drain.
        for (std::size_t i = m.ui.frozen_through;
             i + 1 < m.d.current.messages.size(); ++i) {
            auto& mm = m.d.current.messages[i];
            if (mm.role != Role::Assistant || mm.text.empty()) continue;
            agentty::app::detail::settle_message_md(m, mm);
        }
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        m.ui.pending_settle_freeze = false;
        h.frame(m, "sub" + st + "-submit");

        // Stream the assistant reply, overflowing the viewport. Render
        // with reveal animating between deltas. Do NOT drain reveal at
        // the end — the NEXT loop's submit fires mid-glide.
        Message a; a.role = Role::Assistant;
        a.streaming_text = "Diagram lookup " + st + " begins now:";
        m.d.current.messages.push_back(std::move(a));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        auto& back = m.d.current.messages.back();
        for (int d = 0; d < 30; ++d) {
            back.streaming_text +=
                "\n\nPara " + st + "-" + std::to_string(d)
                + ": builds tree-" + std::to_string(turn * 100 + d)
                + ", lays region-" + std::to_string(turn * 100 + d)
                + ", diffs canvas-" + std::to_string(turn * 100 + d)
                + ", serializes runs batch-" + std::to_string(turn * 100 + d)
                + ".";
            tick(); h.frame(m, "sub" + st + "-d" + std::to_string(d));
            if (h.dead) return;
        }
        // Settle the text into .text (so the next submit's settle path is
        // a no-op shape-wise) but leave reveal possibly still live and do
        // NOT freeze — the next iteration's submit does the freeze.
        back.text += back.streaming_text;
        back.streaming_text.clear();
        // Intentionally NOT calling settle_message_md here — reveal stays
        // live, exactly the pending_settle_freeze state modal.cpp guards.
        m.s.phase = agentty::phase::Idle{};
        m.ui.pending_settle_freeze = true;
        h.frame(m, "sub" + st + "-idle");
    }
    if (pty_master >= 0) close(pty_master);
}

// Code-block fold scenario — the screenshot shape: "Let me check the
// diagram source:" followed by a LONG fenced code block (a mermaid /
// diagram source) that overflows the viewport. auto_fold_long_blocks
// folds a >40-line code block to ~1 row THE MOMENT its closing fence
// commits, WHILE LIVE (turn.cpp). That is a large mid-stream height
// SHRINK over content that has already overflowed into native
// scrollback. If maya re-emits the post-fold (short) canvas from the
// top against the pre-fold (tall) prev_cells, the body lands a second
// time below the committed copy — the visible duplicate. Reveal_fx is
// ON, so the live edge is also animating colored SGR onto the rows that
// scroll off. This is the closest reproduction of the reported bug.
static void run_codeblock_fold_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealcb"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "cb-welcome");

    {
        Message u; u.role = Role::User;
        u.text = "Show me the rendering pipeline diagram.";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        h.frame(m, "cb-submit");
    }

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Let me check the diagram source:";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    for (int f = 0; f < 4; ++f) { tick(); h.frame(m, "cb-open" + std::to_string(f)); }

    auto& back = m.d.current.messages.back();
    // Open a fenced code block, then stream >40 lines so it overflows
    // AND crosses the fold threshold, then CLOSE the fence (commit →
    // live fold to ~1 row).
    back.streaming_text += "\n\n```mermaid";
    tick(); h.frame(m, "cb-fence-open");
    for (int d = 0; d < 70 && !h.dead; ++d) {
        back.streaming_text +=
            "\n  node" + std::to_string(d) + " --> builds tree-"
            + std::to_string(d) + "; diffs canvas-" + std::to_string(d)
            + "; serializes runs batch-" + std::to_string(d) + ";";
        tick(); h.frame(m, "cb-line" + std::to_string(d));
        tick(); h.frame(m, "cb-anim" + std::to_string(d));
    }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }
    // Close the fence → the block commits and folds live.
    back.streaming_text += "\n```\n\nThat is the full pipeline graph.";
    for (int f = 0; f < 6 && !h.dead; ++f) {
        tick(); h.frame(m, "cb-fold" + std::to_string(f));
    }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }

    // Settle + freeze through the production gate.
    {
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        agentty::app::detail::settle_message_md(m, b);
        m.s.phase = agentty::phase::Idle{};
        int guard = 0;
        do {
            tick();
            h.frame(m, "cb-settle" + std::to_string(guard));
        } while (!agentty::app::detail::live_tail_reveal_settled(m)
                 && ++guard < 200 && !h.dead);
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "cb-freeze");
    }
    if (pty_master >= 0) close(pty_master);
}

// TOOL-CARD-GROW-AT-SETTLE scenario — the exact screenshot shape. A
// live assistant run overflows the viewport while a bash tool
// (`git commit`) is still RUNNING (short spinner card). Then the tool
// FAILS, swapping the spinner for a multi-line error body: the card
// GROWS mid-turn, shifting every row above it — including the already-
// overflowed prefix now committed to native scrollback. If maya
// composes that shifted frame once without the follow-up reconciliation
// frames (the clock lapsing at the tool→continuation seam), the old
// card's top rows strand in scrollback = the "card cut off one screen
// up" corruption the user circled. apply_tool_output now arms the
// reconcile cooldown so the clock keeps ticking exactly like
// agent_session's always-on clock; the harness renders those cooldown
// frames and R1/R5/R6 verify no stranded duplicate.
static void run_toolgrow_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealtg"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "tg-welcome");

    // Frozen user turn: committed prefix above the live run.
    {
        Message u; u.role = Role::User;
        u.text = "push all and run the tests";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        h.frame(m, "tg-submit");
    }

    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Assistant streams prose that overflows the viewport, then issues a
    // bash tool that is still RUNNING (short card).
    Message a; a.role = Role::Assistant;
    a.streaming_text = "I'll push both repos and run the suite:";
    m.d.current.messages.push_back(std::move(a));
    for (int f = 0; f < 4; ++f) { tick(); h.frame(m, "tg-open" + std::to_string(f)); }

    {
        auto& back = m.d.current.messages.back();
        for (int d = 0; d < 30 && !h.dead; ++d) {
            back.streaming_text +=
                "\n\nStep " + std::to_string(d)
                + ": builds tree-" + std::to_string(d)
                + ", diffs canvas-" + std::to_string(d)
                + ", serializes runs batch-" + std::to_string(d) + ".";
            tick(); h.frame(m, "tg-d" + std::to_string(d));
            tick(); h.frame(m, "tg-anim" + std::to_string(d));
        }
    }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }

    // Settle the prose into .text (it stays live — tool continuation is
    // pending) and push a RUNNING bash tool sub-turn (short spinner card).
    {
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        agentty::app::detail::settle_message_md(m, b);
    }
    Message tmsg; tmsg.role = Role::Assistant;
    tmsg.tool_calls.push_back(running_tool("tg"));
    m.d.current.messages.push_back(std::move(tmsg));
    m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
    for (int f = 0; f < 5; ++f) { tick(); h.frame(m, "tg-running" + std::to_string(f)); }
    if (h.dead) { if (pty_master >= 0) close(pty_master); return; }

    // THE SEAM: the tool FAILS. apply_tool_output swaps the spinner for a
    // multi-line error body — the card GROWS. Production arms the
    // reconcile cooldown here; model that by rendering the cooldown
    // frames (the clock keeps ticking) so maya reconciles the shifted
    // overflow prefix instead of composing once and stranding it.
    {
        const auto id = m.d.current.messages.back().tool_calls.front().id;
        agentty::app::detail::apply_tool_output(
            m, id,
            std::unexpected(agentty::tools::ToolError::subprocess(
                "[subprocess failed] git_commit (add) failed (exit 128): "
                "fatal: pathspec did not match any files")));

        // apply_tool_output armed settle_cooldown_ticks. Render exactly
        // those cooldown frames (meta.cpp decrements one per Tick) — the
        // window where maya reconciles the grown card. Without the fix
        // the clock lapses after ONE frame and the shifted prefix strands.
        int guard = 0;
        while (m.ui.settle_cooldown_ticks > 0 && guard++ < 40 && !h.dead) {
            --m.ui.settle_cooldown_ticks;   // mirror meta.cpp Tick
            tick();
            h.frame(m, "tg-reconcile" + std::to_string(guard));
        }
        // The turn then settles to Idle; a few idle frames confirm the
        // grown card and composer are stable, single-copy.
        m.s.phase = agentty::phase::Idle{};
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "tg-freeze");
        for (int f = 0; f < 4 && !h.dead; ++f) {
            tick(); h.frame(m, "tg-idle" + std::to_string(f));
        }
    }
    if (pty_master >= 0) close(pty_master);
}

// TRIM-STORM scenario — the FRONT-DELETION path that the fix corrects.
// Every other scenario overflows the viewport but keeps the frozen
// prefix UNDER frozen_row_budget(), so trim_frozen_if_oversized never
// drops entries from the FRONT — it returns none() and the render-time
// gate handles the bottom-shrink collapse. That left the trim's
// commit_scrollback(N) accounting completely un-exercised end-to-end.
//
// Here we run MANY complete turns, each tall enough that the frozen
// prefix blows past frozen_row_budget() (max(48, term_h*3)). Once it
// does, the trim drops the oldest entries from the front — a TOP-
// deletion maya cannot reconcile at render time — and issues
// commit_scrollback(N). If N under-counts the rows actually dropped
// (the old 8192-width conservative estimate), the dropped entries'
// physical rows strand in native scrollback: R6 sees the composer /
// prior turn duplicated one screen up. If N is exact (the fix), the
// boundary lands once and R1/R5/R6 stay clean across every trim.
static void run_trimstorm_scenario(int width, int term_h) {
    setenv("LINES", std::to_string(term_h).c_str(), 1);
    setenv("COLUMNS", std::to_string(width).c_str(), 1);
    const int pty_master = install_pty_stdout(width, term_h);

    Model m;
    m.d.current.id = agentty::ThreadId{"revealts"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Harness h(width, term_h);
    h.frame(m, "ts-welcome");

    // Enough turns that the accumulated frozen prefix exceeds
    // frozen_row_budget() several times over, forcing repeated
    // front-trims. Each turn: user submit (frozen), assistant streams a
    // body that overflows the viewport, settle + freeze + trim.
    const int kTurns = 14;
    for (int turn = 0; turn < kTurns && !h.dead; ++turn) {
        const std::string st = std::to_string(turn);

        {
            Message u; u.role = Role::User;
            u.text = "Turn " + st + ": walk the whole pipeline in detail please.";
            m.d.current.messages.push_back(std::move(u));
            agentty::app::detail::freeze_through(m, m.d.current.messages.size());
            h.frame(m, "ts" + st + "-submit");
        }

        Message a; a.role = Role::Assistant;
        a.streaming_text = "Beginning turn " + st + " investigation:";
        m.d.current.messages.push_back(std::move(a));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        for (int f = 0; f < 3; ++f) { tick(); h.frame(m, "ts" + st + "-open" + std::to_string(f)); }

        auto& back = m.d.current.messages.back();
        // ~1.2 viewports of body per turn so a couple of turns already
        // exceed the budget and every later turn triggers a front-trim.
        const int body_lines = term_h + term_h / 4 + 6;
        for (int d = 0; d < body_lines && !h.dead; ++d) {
            back.streaming_text +=
                "\n\nStep " + st + "." + std::to_string(d)
                + ": builds tree-" + std::to_string(turn * 1000 + d)
                + ", diffs canvas-" + std::to_string(turn * 1000 + d)
                + ", serializes runs batch-" + std::to_string(turn * 1000 + d) + ".";
            tick(); h.frame(m, "ts" + st + "-d" + std::to_string(d));
        }
        if (h.dead) break;

        // Settle + freeze + trim through the production gate.
        auto& b = m.d.current.messages.back();
        b.text += b.streaming_text;
        b.streaming_text.clear();
        agentty::app::detail::settle_message_md(m, b);
        m.s.phase = agentty::phase::Idle{};
        int guard = 0;
        do {
            tick();
            h.frame(m, "ts" + st + "-settle" + std::to_string(guard));
        } while (!agentty::app::detail::live_tail_reveal_settled(m)
                 && ++guard < 200 && !h.dead);
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        // THE PATH UNDER TEST: this trim drops front entries once the
        // prefix exceeds the budget and returns commit_scrollback(N).
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        h.apply_trim(trim);
        h.frame(m, "ts" + st + "-freeze");
        // A few idle frames so any stranded duplicate becomes visible to
        // R6 while the composer repaints post-trim.
        for (int f = 0; f < 3 && !h.dead; ++f) { tick(); h.frame(m, "ts" + st + "-idle" + std::to_string(f)); }
    }
    if (pty_master >= 0) close(pty_master);
}

int main() {
    // Each scenario dup2's a PTY over STDOUT to give term_dims() real
    // geometry; save the original so the summary is visible afterward.
    const int saved_stdout = dup(STDOUT_FILENO);
    std::fprintf(stderr, "reveal_scrollback_test\n");
    const struct { int w, th; } shapes[] = {
        {80, 24}, {60, 16}, {100, 20}, {72, 12},
    };
    // TALL terminals (> 24 rows) exercise the min(rows,24) clamp in
    // view.cpp: the layout is BUILT under a 24-row cap but RENDERED at
    // the real height, so the composer's absolute row in the built tree
    // can disagree with where it lands on the wire. The reported bug is
    // a composer stranded one screen up after a turn settles on exactly
    // such a terminal — R6 catches it.
    const struct { int w, th; } tall_shapes[] = {
        {80, 40}, {100, 50}, {120, 32}, {90, 60},
    };
    for (auto s : shapes) {
        run_scenario(s.w, s.th);
        if (g_failures) break;
    }
    for (auto s : shapes) {
        if (g_failures) break;
        run_multiturn_scenario(s.w, s.th);
    }
    for (auto s : shapes) {
        if (g_failures) break;
        run_submit_mid_reveal_scenario(s.w, s.th);
    }
    for (auto s : shapes) {
        if (g_failures) break;
        run_codeblock_fold_scenario(s.w, s.th);
    }
    // Re-run every scenario on TALL terminals — the untested regime
    // where view.cpp's min(rows,24) build-vs-render height clamp bites.
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_scenario(s.w, s.th);
    }
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_multiturn_scenario(s.w, s.th);
    }
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_codeblock_fold_scenario(s.w, s.th);
    }
    // The reported bug: a PRIOR turn's tall settled write card in the
    // frozen prefix, then a second turn settles above it. Composer
    // duplication (R6) triggers here where the plain-prose scenarios
    // above stay clean.
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_prior_writecard_scenario(s.w, s.th);
    }
    for (auto s : shapes) {
        if (g_failures) break;
        run_prior_writecard_scenario(s.w, s.th);
    }
    // The screenshot bug: a bash tool FAILS mid-turn while the live tail
    // has overflowed the viewport — the card grows by its error body,
    // shifting the committed prefix. Run on both normal and TALL shapes.
    for (auto s : shapes) {
        if (g_failures) break;
        run_toolgrow_scenario(s.w, s.th);
    }
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_toolgrow_scenario(s.w, s.th);
    }
    // The FRONT-DELETION trim path: many tall turns overflow the frozen
    // budget and force real front-trims whose commit_scrollback(N) must
    // be EXACT. Under-commit strands a duplicate (R6); the fix makes N
    // exact. Both normal and TALL shapes.
    for (auto s : shapes) {
        if (g_failures) break;
        run_trimstorm_scenario(s.w, s.th);
    }
    for (auto s : tall_shapes) {
        if (g_failures) break;
        run_trimstorm_scenario(s.w, s.th);
    }
    if (saved_stdout >= 0) { dup2(saved_stdout, STDOUT_FILENO); close(saved_stdout); }
    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::fprintf(stderr, "FAILED\n"); return 1; }
    std::fprintf(stderr, "PASSED\n");
    return 0;
}
