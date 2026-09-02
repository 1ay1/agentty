// test_ghost_caret_runtime.cpp — REPRO at the FULL RUNTIME level for the
// "ghost caret on line 2" report (test + diagnosis credit: davidwed,
// agentty PR #24):
//
//   fill the composer line to the wrap edge → type 1 char (composer
//   wraps to a 2nd visual row) → backspace (composer shrinks back to 1
//   visual row). User sees the painted caret still on the (erased)
//   second row until the next repaint.
//
// Drives the REAL agentty view() + update() + REAL maya Runtime::render
// through a PTY into a compact VT emulator, then inspects the SCREEN
// rows: after the shrink, the caret block must be back on the line
// above, and the previously-caret-holding row must be empty.
//
// Build: link against the agentty tests lib (see CMake agentty_test()).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <clocale>
#include <string>
#include <vector>

#include <fcntl.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <unistd.h>

#include <maya/app/app.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/terminal/tmux.hpp>   // reset_cache_for_test — the probe is one-shot

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/runtime/view/composer.hpp"

using agentty::Model;
using agentty::Msg;

// ── Compact VT emulator (screen + cursor only; scrollback not needed) ──
struct VtEmu {
    int w, h;
    int cx = 0, cy = 0;
    std::vector<std::string> screen;
    bool decawm = true;
    bool cursor_hidden = false;   // DECTCEM (?25l/?25h)

    VtEmu(int c, int r)
        : w(c), h(r), screen(static_cast<std::size_t>(r),
                             std::string(static_cast<std::size_t>(c), ' ')) {}

    void newline() {
        ++cy;
        if (cy >= h) {
            screen.erase(screen.begin());
            screen.push_back(std::string(static_cast<std::size_t>(w), ' '));
            cy = h - 1;
        }
    }
    void put_cp(char32_t cp, int wd) {
        if (wd <= 0) return;
        if (cx + wd > w) {
            if (decawm) { cx = 0; newline(); }
            else cx = w - wd;
        }
        if (cy >= h) newline();
        char vis = (cp < 128) ? static_cast<char>(cp) : '?';
        // We only need geometry + ASCII; render the block glyph as '#'.
        if (cp == 0x2588) vis = '#';
        else if (cp >= 0x2500 && cp <= 0x25FF) vis = '?';
        screen[static_cast<std::size_t>(cy)]
              [static_cast<std::size_t>(cx)] = vis;
        for (int k = 1; k < wd; ++k)
            screen[static_cast<std::size_t>(cy)]
                  [static_cast<std::size_t>(cx + k)] = '?';
        cx += wd;
    }

    void feed(const std::string& s) {
        std::size_t i = 0, n = s.size();
        std::string carry;
        while (i < n || !carry.empty()) {
            char32_t cp = 0; int len = 0;
            if (!carry.empty()) {
                carry += (i < n) ? std::string(1, s[i]) : std::string();
                if (i >= n && carry.size() < 4) break;   // incomplete tail
                // decode from carry
                unsigned char c0 = static_cast<unsigned char>(carry[0]);
                len = (c0 < 0x80) ? 1 : (c0 >> 5) == 0x6 ? 2 :
                      (c0 >> 4) == 0xE ? 3 : (c0 >> 3) == 0x1E ? 4 : 1;
                if (carry.size() < static_cast<std::size_t>(len)) {
                    if (i < n) { carry += s[i]; ++i; continue; }
                    break;
                }
                cp = (c0 < 0x80) ? c0 : (c0 & (0xff >> (len + 1)));
                for (int k = 1; k < len; ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(carry[static_cast<std::size_t>(k)]) & 0x3F);
                carry.clear();
                if (i < n) ++i;
            } else {
                unsigned char c0 = static_cast<unsigned char>(s[i]);
                if (c0 == 0x1B) {
                    // CSI parse
                    if (i + 1 >= n) break;
                    if (s[i + 1] == '[') {
                        std::size_t j = i + 2;
                        std::string params;
                        while (j < n && ((s[j] >= '0' && s[j] <= '9')
                                      || s[j] == ';' || s[j] == '?')) {
                            params += s[j]; ++j;
                        }
                        // Intermediate bytes (0x20-0x2F): e.g. the SP in
                        // DECSCUSR `CSI 5 SP q`. Consume so the final
                        // byte lands in `fin`, not printed as a glyph.
                        while (j < n && s[j] >= 0x20 && s[j] <= 0x2F) ++j;
                        if (j >= n) { carry.clear(); break; }  // partial CSI
                        char fin = s[j];
                        i = j + 1;
                        auto num = [&](int def) {
                            if (params.empty()) return def;
                            std::string f;
                            for (char p : params) { if (p == ';') break; if (p != '?') f += p; }
                            return f.empty() ? def : std::atoi(f.c_str());
                        };
                        auto num_at = [&](int idx, int def) {
                            int cur = 0; std::string f;
                            for (char p : params) {
                                if (p == ';') { if (cur == idx) break; ++cur; f.clear(); }
                                else if (p != '?') f += p;
                            }
                            return f.empty() ? def : std::atoi(f.c_str());
                        };
                        switch (fin) {
                            case 'A': cy -= num(1); if (cy < 0) cy = 0; break;
                            case 'B': cy += num(1); if (cy > h - 1) cy = h - 1; break;
                            case 'C': cx += num(1); if (cx > w - 1) cx = w - 1; break;
                            case 'D': cx -= num(1); if (cx < 0) cx = 0; break;
                            case 'H': case 'f': {
                                int r = num_at(0, 1), c2 = num_at(1, 1);
                                cy = r - 1; cx = c2 - 1;
                                if (cy < 0) cy = 0; if (cy > h - 1) cy = h - 1;
                                if (cx < 0) cx = 0; if (cx > w - 1) cx = w - 1;
                                break;
                            }
                            case 'K': {
                                int m = num(0);
                                auto& l = screen[static_cast<std::size_t>(cy)];
                                if (m == 0)      for (int x = cx; x < w; ++x) l[static_cast<std::size_t>(x)] = ' ';
                                else if (m == 1) for (int x = 0; x <= cx; ++x) l[static_cast<std::size_t>(x)] = ' ';
                                else             l.assign(static_cast<std::size_t>(w), ' ');
                                break;
                            }
                            case 'J': {
                                int m = num(0);
                                if (m == 0) {
                                    auto& l = screen[static_cast<std::size_t>(cy)];
                                    for (int x = cx; x < w; ++x) l[static_cast<std::size_t>(x)] = ' ';
                                    for (int y = cy + 1; y < h; ++y)
                                        screen[static_cast<std::size_t>(y)]
                                              .assign(static_cast<std::size_t>(w), ' ');
                                } else if (m == 1) {
                                    for (int y = 0; y < cy; ++y)
                                        screen[static_cast<std::size_t>(y)]
                                              .assign(static_cast<std::size_t>(w), ' ');
                                    auto& l = screen[static_cast<std::size_t>(cy)];
                                    for (int x = 0; x <= cx; ++x) l[static_cast<std::size_t>(x)] = ' ';
                                } else {
                                    for (auto& l : screen)
                                        l.assign(static_cast<std::size_t>(w), ' ');
                                }
                                break;
                            }
                            case 'h':
                                if (params.find("?7") != std::string::npos) decawm = true;
                                if (params.find("?25") != std::string::npos) cursor_hidden = false;
                                break;
                            case 'l':
                                if (params.find("?7") != std::string::npos) decawm = false;
                                if (params.find("?25") != std::string::npos) cursor_hidden = true;
                                break;
                            case 'm': case 'r': case 's': case 'u': case 't': default: break;
                        }
                        continue;
                    }
                    i += 2;   // other escape: skip 2 bytes
                    continue;
                }
                if (c0 == '\r') { cx = 0; ++i; continue; }
                if (c0 == '\n') { newline(); ++i; continue; }
                if (c0 == '\x08') { if (cx > 0) --cx; ++i; continue; }
                if (c0 == '\x07') { ++i; continue; }
                // UTF-8 decode from the stream (complete sequences only).
                len = (c0 < 0x80) ? 1 : (c0 >> 5) == 0x6 ? 2 :
                      (c0 >> 4) == 0xE ? 3 : (c0 >> 3) == 0x1E ? 4 : 1;
                if (i + static_cast<std::size_t>(len) > n) {
                    carry = s.substr(i);
                    break;
                }
                cp = (c0 < 0x80) ? c0 : (c0 & (0xff >> (len + 1)));
                for (int k = 1; k < len; ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
                i += static_cast<std::size_t>(len);
            }
            int wd = (cp < 128) ? 1 : wcwidth(static_cast<wchar_t>(cp));
            if (wd < 0) wd = 1;
            put_cp(cp, wd);
        }
    }

    std::string row(int y) const {
        std::string r = screen[static_cast<std::size_t>(y)];
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }
};

static std::string read_all(int fd) {
    std::string s;
    char buf[8192];
    for (;;) {
        ssize_t k = ::read(fd, buf, sizeof(buf));
        if (k > 0) { s.append(buf, static_cast<std::size_t>(k)); continue; }
        break;
    }
    return s;
}

static void tick(int ms = 20) {
    maya::testing::advance_anim_clock_ms(ms);
}

static void dump(const VtEmu& emu, const char* why) {
    std::fprintf(stderr, "--- screen after %s ---\n", why);
    for (int y = 0; y < emu.h; ++y) {
        std::string r = emu.row(y);
        std::fprintf(stderr, "  %2d|%s|\n", y, r.c_str());
    }
}

int main() {
    // Make the Witness-Chain gate tripwire FATAL: any committed-prefix
    // corruption (the scrollback-safety property a mis-parked cursor
    // would violate) aborts loudly instead of soft-recovering.
    setenv("MAYA_GATE_ABORT", "1", 1);
    setenv("MAYA_NO_SYNC", "1", 1);   // no synchronized-output wrapper — unsynced wire
    // Force the hardware-caret path: under tmux the composer falls back
    // to the painted caret (tmux copy-mode ghosts a live cursor), so
    // the hardware-caret assertions below require a tmux-free env.
    // The tmux fallback itself is covered by the painted-mode run
    // (AGENTTY_PAINTED_CARET=1) which shares the composer's painted
    // path.
    unsetenv("TMUX");
    setenv("TERM", "xterm-256color", 1);
    setlocale(LC_ALL, "C.UTF-8");

    // tmux presence is toggled through the FAKE probe seam, never by
    // setting $TMUX alone. Two reasons, both of which bit this test:
    //
    //   1. maya's tmux probe is ONE-SHOT and cached per process. Setting
    //      $TMUX after the first composer_config() call changes nothing,
    //      because the probe already answered "not in tmux" and froze.
    //      Every tmux assertion below was silently running tmux-FREE.
    //   2. Without MAYA_TMUX_FAKE the probe shells out to the developer's
    //      REAL tmux server, so the result depends on whose machine and
    //      whose config is loaded — green on a box whose termfeatures
    //      advertise cstyle, red on a box without a server. A capability
    //      test must not ask the developer's environment for the answer.
    //
    // The feature list mirrors what a modern tmux reports; cstyle is the
    // one the caret gate actually consults (DECSCUSR shaping).
    auto set_tmux = [](bool on) {
        if (on) {
            setenv("TMUX", "/tmp/tmux-0/default,1,0", 1);
            setenv("MAYA_TMUX_FAKE", "1", 1);
            setenv("MAYA_TMUX_FEATURES",
                   "ccolour,clipboard,cstyle,extkeys,focus,RGB,sync", 1);
        } else {
            unsetenv("TMUX");
            unsetenv("MAYA_TMUX_FAKE");
            unsetenv("MAYA_TMUX_FEATURES");
        }
        maya::tmux::reset_cache_for_test();   // re-probe under the new env
    };
    set_tmux(false);

    // ── Gate unit-check ─────────────────────────────────────
    // The composer only shows the hardware caret when it is safe. Each
    // gate below fixes a real ghost-cursor report; assert the decision
    // directly (cheaper + more precise than driving the whole app).
    // Skipped under the AGENTTY_PAINTED_CARET wholesale opt-out (which
    // forces every gate off) — the painted-mode run covers that path.
    if (std::getenv("AGENTTY_PAINTED_CARET") == nullptr) {
        Model g;
        g.d.current.messages.push_back({});
        g.d.current.messages.back().role = agentty::Role::User;
        g.ui.composer.text = "hi";
        g.ui.composer.cursor = 2;
        g.ui.terminal_focused = true;
        g.s.phase = agentty::phase::Idle{};

        // Base case (tmux-free, focused, idle): hardware caret ON.
        set_tmux(false);
        if (!agentty::ui::composer_config(g).hardware_caret) {
            std::fprintf(stderr, "FAIL(gate): hardware caret off in the "
                         "safe base case (tmux-free, focused, idle)\n");
            return 1;
        }
        // tmux + idle: caret ON. The copy-mode ghost is a streaming-
        // plus-scroll problem; an idle screen is static, so tmux's
        // copy-mode cursor tracks a stable position and never lands on
        // shifting content. (Earlier this was force-hidden under tmux,
        // leaving the common idle state caret-LESS — "tmux doesn't have
        // the hardware caret.") The right-margin park handles the
        // streaming-scroll case; idle no longer pays for it.
        set_tmux(true);
        if (!agentty::ui::composer_config(g).hardware_caret) {
            std::fprintf(stderr, "FAIL(gate): hardware caret OFF under tmux "
                         "while idle — idle is static, no copy-mode ghost\n");
            return 1;
        }
        set_tmux(false);
        // Unfocused terminal: off (no blinking bar in an inactive pane).
        g.ui.terminal_focused = false;
        if (agentty::ui::composer_config(g).hardware_caret) {
            std::fprintf(stderr, "FAIL(gate): hardware caret ON while the "
                         "terminal window is unfocused\n");
            return 1;
        }
        g.ui.terminal_focused = true;
        // tmux + streaming with no recent edit: still OFF (rows scroll;
        // a reading user scrolled up must not have a caret re-aimed onto
        // thread content — the ghost the right-margin park mitigates but
        // doesn't fully remove while the screen is moving). Verify the
        // gate holds under tmux specifically, not just tmux-free.
        set_tmux(true);
        g.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        g.ui.composer.last_edit_ms = maya::anim::default_clock().now_ms();
        maya::testing::advance_anim_clock_ms(10'000);   // 10 s later
        if (agentty::ui::composer_config(g).hardware_caret) {
            std::fprintf(stderr, "FAIL(gate): hardware caret ON while "
                         "streaming with no recent edit (under tmux)\n");
            return 1;
        }
        set_tmux(false);
        // …but a RECENT edit during streaming keeps it on (you're typing
        // a queued message, not scrolled up reading). tmux-free for this
        // one so an outer `env TMUX=…` can't mask it.
        g.ui.composer.last_edit_ms = maya::anim::default_clock().now_ms();
        maya::testing::advance_anim_clock_ms(500);      // 0.5 s later
        if (!agentty::ui::composer_config(g).hardware_caret) {
            std::fprintf(stderr, "FAIL(gate): hardware caret OFF while "
                         "streaming but actively typing\n");
            return 1;
        }
        std::fprintf(stderr, "PASS(gate): caret on when idle (incl. tmux) "
                     "and while typing; off when unfocused or "
                     "streaming-idle.\n");
    }

    // Small terminal so the composer text wraps at a reachable edge AND
    // the whole frame (thread + composer + status bar) OVERFLOWS the
    // viewport — the composer rides the bottom edge, so the wrap frame
    // scrolls a row into native scrollback and the shrink frame must
    // erase past committed rows. That is the geometry the bug report
    // comes from (long conversation, composer pinned at the bottom).
    const int W = 46, H = 10;
    const int H2 = 16;   // resize-phase height (pad variant)

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed\n"); return 2;
    }
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(W);
    ws.ws_row = static_cast<unsigned short>(H);
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    ::close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    maya::RunConfig cfg;
    cfg.mode = maya::Mode::Inline;
    auto rt_r = maya::detail::Runtime::create(cfg);
    if (!rt_r) { std::fprintf(stderr, "Runtime::create failed\n"); return 2; }
    maya::detail::Runtime& rt = *rt_r;

    Model m;
    // Welcome screen off → plain composer-only frame.
    m.d.current.messages.push_back({});
    m.d.current.messages.back().role = agentty::Role::User;

    VtEmu emu(W, H2);   // sized for the LATER resize phase (H2=16)

    auto frame = [&](const char* tag) {
        tick(20);
        maya::Element root = agentty::ui::view(m);
        (void)rt.render(root);
        tick(20);
        emu.feed(read_all(master));
        (void)tag;
    };
    auto type = [&](char32_t ch) {
        auto [m2, cmd] = agentty::app::update(std::move(m),
                                              Msg{agentty::ComposerCharInput{ch}});
        m = std::move(m2);
        (void)cmd;
    };
    auto backspace = [&]() {
        auto [m2, cmd] = agentty::app::update(std::move(m),
                                              Msg{agentty::ComposerBackspace{}});
        m = std::move(m2);
        (void)cmd;
    };

    // ── Baseline: empty composer ──────────────────────────────────────
    frame("initial");
    dump(emu, "initial");

    // Fill the line ONE CHAR PER FRAME until the composer visually
    // wraps (caret drops to a row below the first text row). This mirrors
    // the user's keystroke cadence: every char is its own frame, so the
    // wrap-inducing char is the ONLY delta of its frame.
    int first_text_row = -1, caret_row = -1, caret_col = -1;
    auto scan = [&]() {
        first_text_row = caret_row = caret_col = -1;
        // The caret is EITHER the visible hardware cursor (hardware-
        // caret mode — the default since the maya CursorHint work) OR a
        // painted '#' block (painted mode / AGENTTY_PAINTED_CARET).
        // Both map to "exactly one caret on screen"; the assertions
        // below are mode-agnostic.
        if (!emu.cursor_hidden) {
            caret_row = emu.cy;
            caret_col = emu.cx;
        }
        for (int y = 0; y < H; ++y) {
            std::size_t p = emu.screen[static_cast<std::size_t>(y)].find('#');
            if (p != std::string::npos
                && caret_row < 0) { caret_row = y; caret_col = static_cast<int>(p); }
        }
        for (int y = 0; y < H; ++y) {
            std::string r = emu.row(y);
            if (r.find("BBBB") != std::string::npos) { first_text_row = y; break; }
        }
    };
    // Seed one word so first_text_row is findable.
    for (const char* p = "BBBB"; *p; ++p) type(static_cast<char32_t>(*p));
    frame("seed");
    for (int guard = 0; guard < 80; ++guard) {
        type(U'B');
        frame("typed-1");
        scan();
        if (first_text_row >= 0 && caret_row > first_text_row) break;   // wrapped
    }
    dump(emu, "after fill (wrapped)");
    if (caret_row < 0) {
        std::fprintf(stderr, "FAIL: no caret visible after fill\n");
        return 1;
    }
    std::fprintf(stderr, "after fill: caret at row %d col %d "
                 "(first text row %d)\n", caret_row, caret_col, first_text_row);

    // The wrapped state: caret row > first text row.
    if (first_text_row < 0 || caret_row <= first_text_row) {
        std::fprintf(stderr, "SKIP: composer did not wrap (caret row %d, "
                             "first text row %d)\n", caret_row, first_text_row);
        return 77;
    }

    // ── The backspace ─────────────────────────────────────────────────
    backspace();
    frame("backspace");

    int br = -1, bc = -1;
    auto scan_caret = [&](int& r, int& c) {
        r = c = -1;
        if (!emu.cursor_hidden) { r = emu.cy; c = emu.cx; return; }
        for (int y = 0; y < H; ++y) {
            std::size_t p = emu.screen[static_cast<std::size_t>(y)].find('#');
            if (p != std::string::npos) { r = y; c = static_cast<int>(p); return; }
        }
    };
    scan_caret(br, bc);
    if (br < 0) {
        std::fprintf(stderr, "FAIL: caret vanished after backspace\n");
        return 1;
    }
    std::fprintf(stderr, "after backspace: caret at row %d col %d\n", br, bc);

    if (br > first_text_row) {
        dump(emu, "BUG state");
        std::fprintf(stderr,
            "\nBUG REPRODUCED: caret still on the wrapped row %d after the "
            "shrink (composer is single-row again).\n", br);
        return 42;
    }

    // Also verify: the row the caret used to occupy must be gone/blank.
    if (br == caret_row) {
        dump(emu, "BUG state");
        std::fprintf(stderr,
            "\nBUG (variant): caret did not move rows on shrink "
            "(row %d before and after).\n", caret_row);
        return 43;
    }

    // ── Oscillation: type/backspace cycles with blink frames between.
    // The blink toggles the caret cell every 530 ms; drive several
    // cycles so a stale diff slot has every chance to surface.
    for (int cycle = 0; cycle < 6; ++cycle) {
        type(U'B');
        frame("osc-type");
        backspace();
        tick(540);           // cross a blink toggle boundary
        frame("osc-backspace+blink");
        scan_caret(br, bc);
        if (br < 0) {
            std::fprintf(stderr, "FAIL: caret vanished in cycle %d\n", cycle);
            return 1;
        }
        if (br > first_text_row) {
            dump(emu, "BUG state (osc)");
            std::fprintf(stderr,
                "\nBUG REPRODUCED (cycle %d): caret on wrapped row %d after "
                "shrink.\n", cycle, br);
            return 42;
        }
        // Stale-caret DUPLICATE check, mode-agnostic. Hardware-caret
        // mode: the terminal's own cursor IS the caret — it must be
        // visible and there must be ZERO painted '#' blocks (any '#' is
        // a stale painted cell). Painted mode: exactly one '#'.
        int n = 0;
        for (int y = 0; y < H; ++y)
            for (char ch : emu.screen[static_cast<std::size_t>(y)])
                if (ch == '#') ++n;
        const int want = emu.cursor_hidden ? 1 : 0;
        if (n != want) {
            dump(emu, "BUG state (osc, dup)");
            std::fprintf(stderr,
                "\nBUG (variant, cycle %d): %d painted caret block(s) on "
                "screen (expected %d; hardware cursor %s).\n",
                cycle, n, want, emu.cursor_hidden ? "hidden" : "visible");
            return 43;
        }
    }

    std::fprintf(stderr, "PASS: caret tracks wrap/shrink through %d "
                 "oscillation cycles.\n", 6);

    // ── Streaming variant: same wrap/shrink dance while the composer is
    // in Streaming state — that is the ONLY state where build() routes
    // through the cross-frame ComponentCache (keyed on cache_id, which
    // folds text+cursor+state, so a cached blit must never serve stale
    // wrap geometry).
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Type until wrapped again.
    scan();
    for (int guard = 0; guard < 80; ++guard) {
        type(U'B');
        frame("stream-type");
        scan();
        if (first_text_row >= 0 && caret_row > first_text_row) break;
    }
    if (!(first_text_row >= 0 && caret_row > first_text_row)) {
        std::fprintf(stderr, "SKIP: streaming composer did not wrap\n");
        return 78;
    }
    std::fprintf(stderr, "streaming wrapped: caret row %d (first text row %d)\n",
                 caret_row, first_text_row);
    const int s_first_text_row = first_text_row;
    const int s_caret_row = caret_row;

    backspace();
    frame("stream-backspace");
    scan_caret(br, bc);
    if (br < 0) {
        std::fprintf(stderr, "FAIL: caret vanished after streaming backspace\n");
        return 1;
    }
    std::fprintf(stderr, "after streaming backspace: caret at row %d col %d\n",
                 br, bc);
    if (br > s_first_text_row) {
        dump(emu, "BUG state (streaming shrink)");
        std::fprintf(stderr,
            "\nBUG REPRODUCED (streaming): caret on wrapped row %d after "
            "shrink.\n", br);
        return 44;
    }
    if (br == s_caret_row) {
        dump(emu, "BUG state (streaming, no move)");
        std::fprintf(stderr,
            "\nBUG (variant, streaming): caret did not move on shrink.\n");
        return 45;
    }

    // Two more cycles in streaming (cache warm now).
    for (int cycle = 0; cycle < 2; ++cycle) {
        type(U'B');
        frame("stream-osc-type");
        backspace();
        frame("stream-osc-backspace");
        scan_caret(br, bc);
        if (br < 0 || br > s_first_text_row) {
            dump(emu, "BUG state (streaming osc)");
            std::fprintf(stderr,
                "\nBUG REPRODUCED (streaming cycle %d): caret row %d.\n",
                cycle, br);
            return 44;
        }
    }

    std::fprintf(stderr, "PASS: streaming-state (component-cache) path "
                 "tracks wrap/shrink too.\n");

    // ── Pad-variant (the actual user report): in a FITTING terminal the
    // composer does NOT sit in the overflow band, so maya's composer
    // anti-bounce hold (inline_min_content pad, kMaxHoldPad=2) is free to
    // bridge the 1-row dip the backspace produces. The pad decays after
    // kHoldDecayFrames=6 NON-RISING frames — but only in frames that
    // actually render. After the last backspace the ONLY render driver is
    // the idle caret blink (~4 renders/s) — and once blink stops (15 s
    // idle), NOTHING renders: a stranded pad row = the "composer still
    // two lines tall / cursor on the ghost second line" report.
    //
    // Reproduce: new small session in a taller terminal, wrap, backspace,
    // then drive ~12 frames (enough for the hold to decay) and assert the
    // pad row is GONE — i.e. the status bar's top border sits DIRECTLY
    // below the composer's bottom border.
    {
        struct winsize ws2{};
        ws2.ws_col = (unsigned short)W;
        ws2.ws_row = (unsigned short)H2;
        ioctl(master, TIOCSWINSZ, &ws2);
        // maya picks the resize up on the next frame via its width-
        // backstop / SIGWINCH path.
        tick(50);
        frame("resize-tall");

        m.s.phase = agentty::phase::Idle{};
        m.ui.composer.text.clear();
        m.ui.composer.cursor = 0;
        m.ui.composer.history_idx = -1;
        m.ui.composer.draft_save.reset();
        frame("cleared");

        int prev_bottom = -1;
        auto composer_bottom = [&]() {
            // The composer's bottom border row: the LAST row holding the
            // box bottom-left corner '╰' (rendered '?'). Find the last
            // row containing a '?', skipping the status bar (its rows
            // also have '?'); the composer's bottom border is the row
            // immediately above the status bar's top border. We identify
            // the status bar's TOP border row as the second-to-last
            // bordered row and take the one above it.
            int bordered[3] = {-1, -1, -1};
            int nb = 0;
            for (int y = H2 - 1; y >= 0 && nb < 3; --y) {
                std::size_t p = emu.screen[static_cast<std::size_t>(y)].find('?');
                if (p != std::string::npos) bordered[nb++] = y;
            }
            // bordered[0]=status top, bordered[1]=composer bottom,
            // bordered[2]=composer top.
            return bordered[1];
        };
        prev_bottom = composer_bottom();
        if (prev_bottom < 0) {
            std::fprintf(stderr, "FAIL: composer bottom border not found after clear\n");
            return 1;
        }

        // Wrap it again.
        scan();
        for (int guard = 0; guard < 80; ++guard) {
            type(U'B');
            frame("pad-type");
            scan();
            if (first_text_row >= 0 && caret_row > first_text_row) break;
        }
        if (!(first_text_row >= 0 && caret_row > first_text_row)) {
            std::fprintf(stderr, "SKIP: pad-variant composer did not wrap\n");
            return 77;   // ctest SKIP_RETURN_CODE
        }
        const int wrapped_bottom = composer_bottom();
        if (wrapped_bottom <= prev_bottom) {
            std::fprintf(stderr, "SKIP: composer did not grow on wrap "
                         "(bottom %d -> %d)\n", prev_bottom, wrapped_bottom);
            return 77;   // ctest SKIP_RETURN_CODE
        }
        std::fprintf(stderr, "pad-variant: wrapped bottom row %d (was %d)\n",
                     wrapped_bottom, prev_bottom);

        backspace();
        frame("pad-backspace");
        const int after_bs_bottom = composer_bottom();
        std::fprintf(stderr, "pad-variant: bottom after backspace %d\n",
                     after_bs_bottom);

        // Drive the hold out: 12 more frames with blink cadence. The pad
        // must decay to 0 and the composer bottom must return to
        // prev_bottom. If it stays a row taller, the ghost pad row is
        // stranded — exactly what the user sees.
        for (int f = 0; f < 12; ++f) {
            tick(265);           // blink half-period: real render drivers
            frame("pad-decay");
        }
        const int settled_bottom = composer_bottom();
        std::fprintf(stderr, "pad-variant: settled bottom %d (pre-wrap %d)\n",
                     settled_bottom, prev_bottom);
        if (settled_bottom != prev_bottom) {
            dump(emu, "BUG state (pad ghost row)");
            std::fprintf(stderr,
                "\nBUG REPRODUCED (pad): composer bottom settled at row %d "
                "but pre-wrap height was %d — a ghost pad row survived the "
                "shrink.\n", settled_bottom, prev_bottom);
            return 46;
        }
        std::fprintf(stderr, "PASS: anti-bounce pad decays after the "
                     "shrink (no ghost row).\n");
    }

    // ── WELCOME-SCREEN variant (the user's exact report: "only on FIRST
    // start, once the first prompt is gone it doesn't happen").
    //
    // On a fresh session the thread is EMPTY and Thread::Config routes
    // to the WelcomeScreen (thread.cpp: messages.empty() → is_empty +
    // welcome cfg). That subtree differs from the conversation in three
    // ways that interact with the wrap/shrink dance:
    //   • the welcome is CLAMPED to term_rows-11 (welcome_screen.cpp
    //     kIdleChromeRows) — the composer does NOT ride the viewport
    //     bottom;
    //   • the welcome wordmark Bobs (sine per-letter), which keeps the
    //     visual_hash advancing at its own cadence;
    //   • typing the first char swaps welcome→conversation (the
    //     "starters card" first_run path only exists while threads are
    //     empty).
    // My earlier variants pushed a user message to SUPPRESS the welcome
    // — so none of them exercised this subtree. Reproduce exactly:
    // fresh empty thread (welcome), fill to wrap, ONE backspace, then
    // idle-drive the blink and assert the composer returns to its
    // single-row height.
    for (const int Hw : {H2, 10}) {
        // Small-height pass: welcome (clamped to 4 rows) + composer +
        // status bar OVERFLOW the viewport — the composer rides the
        // bottom edge and the wrap scroll commits rows. That is the
        // first-start-on-a-small-terminal geometry.
        {
            struct winsize wsw{};
            wsw.ws_col = (unsigned short)W;
            wsw.ws_row = (unsigned short)Hw;
            ioctl(master, TIOCSWINSZ, &wsw);
            tick(50);
            frame("welcome-resize");
        }
        m.d.current.messages.clear();          // back to welcome screen
        m.d.threads.clear();                   // first_run starters path
        m.s.threads_loading = false;
        m.s.phase = agentty::phase::Idle{};
        m.ui.composer.text.clear();
        m.ui.composer.cursor = 0;
        m.ui.composer.history_idx = -1;
        m.ui.composer.draft_save.reset();
        frame("welcome-fresh");
        dump(emu, "welcome (first start)");

        auto composer_bottom2 = [&]() {
            int bordered[3] = {-1, -1, -1};
            int nb = 0;
            for (int y = H2 - 1; y >= 0 && nb < 3; --y) {
                std::size_t p = emu.screen[static_cast<std::size_t>(y)].find('?');
                if (p != std::string::npos) bordered[nb++] = y;
            }
            return bordered[1];
        };
        // Scroll-invariant composer height: rows between the composer's
        // BOTTOM border and the STATUS BAR's top border. The welcome
        // wordmark bob can shift absolute rows (its wrapped-tagline rest
        // line scrolls in/out), so absolute anchors misread as a ghost
        // row; this gap can only grow if a real row appears between
        // composer and status bar.
        auto composer_gap = [&]() {
            int status_top = -1, comp_bottom = -1;
            for (int y = H2 - 1; y >= 0; --y) {
                std::size_t p = emu.screen[static_cast<std::size_t>(y)].find('?');
                if (p != std::string::npos) {
                    if (status_top < 0)      status_top = y;
                    else if (comp_bottom < 0) { comp_bottom = y; break; }
                }
            }
            return (status_top >= 0 && comp_bottom >= 0)
                   ? status_top - comp_bottom : -1;
        };
        const int welcome_bottom = composer_bottom2();
        const int welcome_gap    = composer_gap();
        if (welcome_bottom < 0) {
            std::fprintf(stderr, "FAIL: welcome composer bottom not found\n");
            return 1;
        }
        std::fprintf(stderr, "welcome: composer bottom row %d\n", welcome_bottom);

        // Fill until wrapped. NOTE: the FIRST char swaps the welcome
        // subtree out (thread.cpp is_empty → conversation), so the real
        // wrap/shrink dance happens against the conversation view — but
        // the frame that MATTERS for the user report is the one where
        // the welcome was JUST swapped: fresh session, first prompt
        // being typed, transient geometry, swap handoff (the
        // thinking-indicator-style seam dip the AppLayout pad comment
        // names). Drive it identically: type until wrapped.
        scan();
        for (int guard = 0; guard < 80; ++guard) {
            type(U'B');
            frame("welcome-type");
            scan();
            if (first_text_row >= 0 && caret_row > first_text_row) break;
        }
        if (!(first_text_row >= 0 && caret_row > first_text_row)) {
            std::fprintf(stderr, "SKIP: welcome-frame composer did not wrap "
                         "(caret row %d, text row %d)\n",
                         caret_row, first_text_row);
            return 81;
        }
        const int w_bottom = composer_bottom2();
        const int w_gap    = composer_gap();
        std::fprintf(stderr, "welcome: wrapped, bottom %d (was %d), gap %d "
                     "(was %d), caret row %d, text row %d\n",
                     w_bottom, welcome_bottom, w_gap, welcome_gap,
                     caret_row, first_text_row);
        dump(emu, "welcome wrapped");

        // ONE backspace → single row again.
        backspace();
        frame("welcome-backspace");
        dump(emu, "welcome right after backspace");

        // Idle-drive the blink (the ONLY render driver now) for well
        // past kHoldDecayFrames=6 renders.
        for (int f = 0; f < 12; ++f) {
            tick(265);
            frame("welcome-decay");
        }
        dump(emu, "welcome after backspace + idle decay");

        const int settled2 = composer_bottom2();
        const int settled_gap = composer_gap();
        std::fprintf(stderr, "welcome: settled bottom %d (pre-wrap %d), "
                     "gap %d (pre-wrap %d)\n",
                     settled2, welcome_bottom, settled_gap, welcome_gap);
        if (settled_gap != welcome_gap) {
            std::fprintf(stderr,
                "\nBUG REPRODUCED (welcome): composer→status-bar gap settled "
                "at %d rows but pre-wrap was %d — ghost row survives the "
                "first-start shrink.\n", settled_gap, welcome_gap);
            return 47;
        }

        // Caret must be back on the single text row.
        scan_caret(br, bc);
        scan();
        if (br < 0 || (first_text_row >= 0 && br > first_text_row)) {
            std::fprintf(stderr,
                "\nBUG REPRODUCED (welcome caret): caret row %d, text row "
                "%d — the painted caret is still on the wrapped row.\n",
                br, first_text_row);
            return 48;
        }
        std::fprintf(stderr, "PASS: welcome-screen (first start, H=%d) "
                     "wrap/shrink is clean.\n", Hw);
    }

    // ── Exit path: the "agentty eats composer lines on close" repro ───
    // In hardware-caret mode every frame ends with the physical cursor
    // AT THE CARET CELL — inside the composer box, rows above the frame
    // bottom. Runtime::cleanup() must emit the finalize bytes (CUD back
    // to the resting row) BEFORE the Terminal dtor's teardown \r\n;
    // otherwise the shell prompt lands mid-box and its output eats the
    // remaining composer rows from scrollback.
    {
        // Ensure a normal focused idle composer with the caret shown.
        m.s.phase = agentty::phase::Idle{};
        frame("pre-exit");
        scan_caret(br, bc);
        const bool caret_was_shown = !emu.cursor_hidden;
        const int  caret_row_pre   = br;

        // Find the frame's bottom row: the status bar's bottom border is
        // the last non-blank row on screen.
        int frame_bottom = -1;
        for (int y = emu.h - 1; y >= 0; --y) {
            if (emu.row(y).find_first_not_of(' ') != std::string::npos) {
                frame_bottom = y;
                break;
            }
        }

        // Tear down the runtime the way a real quit does.
        (void)rt.cleanup();
        emu.feed(read_all(master));

        // The cursor must now sit AT or BELOW the frame's last row —
        // never inside the composer box — so the shell prompt that
        // follows starts on a fresh line below the transcript.
        if (caret_was_shown && caret_row_pre >= 0 && frame_bottom >= 0
            && emu.cy < frame_bottom) {
            dump(emu, "exit state");
            std::fprintf(stderr,
                "\nBUG REPRODUCED (exit): cleanup left the cursor at row %d "
                "but the frame bottom is row %d — the shell prompt will "
                "overwrite composer rows (the 'eats lines on close' "
                "report).\n", emu.cy, frame_bottom);
            return 49;
        }
        // Simulate the shell prompt the terminal prints next; it must
        // land on a fresh row, not inside the box.
        const int prompt_row = emu.cy;
        emu.feed("$ \r\n");
        if (frame_bottom >= 0 && prompt_row < frame_bottom) {
            std::fprintf(stderr,
                "\nBUG (exit variant): shell prompt rendered at row %d, "
                "inside the frame (bottom %d).\n", prompt_row, frame_bottom);
            return 49;
        }
        std::fprintf(stderr, "PASS: exit path returns the cursor below the "
                     "frame (row %d >= bottom %d) before the shell prompt.\n",
                     prompt_row, frame_bottom);
    }

    return 0;
}
