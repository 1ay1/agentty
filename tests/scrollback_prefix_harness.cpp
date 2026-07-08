// scrollback_prefix_harness — STANDALONE, on-demand append-only-prefix
// oracle for the agentty↔maya scrollback seam.
//
// WHY THIS EXISTS (and why it is NOT a ctest entry):
//   The full oracle (tests/scrollback_oracle_test.cpp) drives maya's real
//   Runtime::render through a PTY with the whole agentty view + freeze/trim
//   lifecycle — thorough, but heavy, and it lives behind AGENTTY_BUILD_TESTS
//   + ctest. This project's workflow skips ctest. So the invariant that
//   actually matters — "a row committed to native scrollback is NEVER later
//   rewritten, reordered, or duplicated" — had no cheap, reachable check.
//
//   This harness fills that gap. It drives maya's PUBLIC inline-frame API
//   (Empty→seed→render→commit→demote→render) directly against a minimal
//   in-process terminal emulator with native scrollback, across a battery
//   of oversized-startup / committed-prefix / re-emit scenarios, and
//   asserts the append-only-prefix property. It builds with ONE g++ line
//   (no CMake, no maya test target) and runs in well under a second:
//
//     g++ -std=c++23 -I maya/include tests/scrollback_prefix_harness.cpp
//         build/maya/libmaya.a -o /tmp/sbh && /tmp/sbh
//
//   (Any libmaya.a / object set providing InlineFrameState + Canvas +
//   Writer works; the harness only touches those three public headers.)
//
//   The minimal TermEmu + pipe writer are ported from
//   test_fresh_oversized_scrollback.cpp — kept self-contained here so the
//   harness has ZERO dependency on the test tree's check.hpp or build glue.

#include "maya/render/inline_frame.hpp"
#include "maya/render/canvas.hpp"
#include "maya/terminal/writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace maya;
using namespace maya::inline_frame;

// ─────────────────────────────────────────────────────────────────────────
// Non-blocking pipe Writer (ported).
// ─────────────────────────────────────────────────────────────────────────
static std::pair<Writer, int> make_pipe_writer() {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::abort(); }
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    Writer w{static_cast<platform::NativeHandle>(fds[1])};
    return {std::move(w), fds[0]};
}

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

// ─────────────────────────────────────────────────────────────────────────
// TermEmu — minimal ANSI terminal with native scrollback (ported).
// ─────────────────────────────────────────────────────────────────────────
struct TermEmu {
    int cols, rows;
    int cx = 0, cy = 0;
    std::vector<std::string> screen;
    std::vector<std::string> scrollback;
    bool decawm = true;

    TermEmu(int c, int r) : cols(c), rows(r) {
        screen.assign(static_cast<std::size_t>(rows),
                      std::string(static_cast<std::size_t>(cols), ' '));
    }
    std::string& line(int y) { return screen[static_cast<std::size_t>(y)]; }

    void put(char ch) {
        if (cx >= cols) { if (decawm) { cx = 0; cursor_newline(); } else cx = cols - 1; }
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols) l.resize(static_cast<std::size_t>(cols), ' ');
        l[static_cast<std::size_t>(cx)] = ch;
        ++cx;
    }
    void cursor_newline() {
        if (cy < rows - 1) { ++cy; return; }
        std::string top = screen.front();
        while (!top.empty() && top.back() == ' ') top.pop_back();
        scrollback.push_back(std::move(top));
        screen.erase(screen.begin());
        screen.push_back(std::string(static_cast<std::size_t>(cols), ' '));
    }
    void carriage_return() { cx = 0; }
    void cursor_up(int n)   { cy -= n; if (cy < 0) cy = 0; }
    void cursor_down(int n) { for (int i = 0; i < n; ++i) cursor_newline(); }
    void cursor_fwd(int n)  { cx += n; if (cx > cols) cx = cols; }
    void erase_to_eol() {
        std::string& l = line(cy);
        if (static_cast<int>(l.size()) < cols) l.resize(static_cast<std::size_t>(cols), ' ');
        for (int x = cx; x < cols; ++x) l[static_cast<std::size_t>(x)] = ' ';
    }
    void erase_to_eos() {
        erase_to_eol();
        for (int y = cy + 1; y < rows; ++y) line(y).assign(static_cast<std::size_t>(cols), ' ');
    }
    void feed(const std::string& b) {
        std::size_t i = 0, n = b.size();
        while (i < n) {
            unsigned char ch = static_cast<unsigned char>(b[i]);
            if (ch == '\r') { carriage_return(); ++i; continue; }
            if (ch == '\n') { cursor_newline(); ++i; continue; }
            if (ch == 0x1b) { i = parse_esc(b, i); continue; }
            if (ch >= 0x20) { put(static_cast<char>(ch < 128 ? ch : '?')); ++i; continue; }
            ++i;
        }
    }
    std::size_t parse_esc(const std::string& b, std::size_t i) {
        const std::size_t n = b.size();
        if (i + 1 >= n) return n;
        if (b[i + 1] != '[') return i + 2;
        std::size_t j = i + 2;
        bool priv = false;
        if (j < n && b[j] == '?') { priv = true; ++j; }
        int param = 0; bool have_param = false;
        while (j < n && b[j] >= '0' && b[j] <= '9') { param = param * 10 + (b[j]-'0'); have_param = true; ++j; }
        if (j >= n) return n;
        char final = b[j];
        const int p = have_param ? param : 0;
        switch (final) {
            case 'A': cursor_up(p ? p : 1); break;
            case 'B': cursor_down(p ? p : 1); break;
            case 'C': cursor_fwd(p ? p : 1); break;
            case 'D': cx -= (p ? p : 1); if (cx < 0) cx = 0; break;
            case 'G': cx = (p ? p - 1 : 0); if (cx < 0) cx = 0; break;
            case 'H': cy = 0; cx = 0; break;
            case 'K': erase_to_eol(); break;
            case 'J':
                if (p == 2 || p == 3) {
                    if (p == 3) scrollback.clear();
                    for (auto& l : screen) l.assign(static_cast<std::size_t>(cols), ' ');
                } else erase_to_eos();
                break;
            case 'h': if (priv && p == 7) decawm = true; break;
            case 'l': if (priv && p == 7) decawm = false; break;
            default: break;
        }
        return j + 1;
    }
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

// ─────────────────────────────────────────────────────────────────────────
// Harness state + assertions.
// ─────────────────────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_checks   = 0;

static void check(bool ok, const std::string& msg) {
    ++g_checks;
    if (!ok) { ++g_failures; std::fprintf(stderr, "  FAIL: %s\n", msg.c_str()); }
}

static Canvas marked_canvas(int width, int rows, StylePool& pool, const char* prefix) {
    Canvas c(width, rows + 4, &pool);
    auto sid = pool.intern(Style{});
    for (int y = 0; y < rows; ++y) {
        std::string label = std::string(prefix) + "-row-" + std::to_string(y);
        c.write_text(0, y, label, sid);
    }
    return c;
}

// The INVARIANT check: no marked row appears twice in the whole transcript
// (scrollback ++ viewport). A duplicate means a committed row was re-emitted
// — the exact corruption class. Returns first dup pair (or {-1,-1}).
struct DupScan { int a = -1, b = -1; std::string line; int count = 0; };
static DupScan scan_dups(const TermEmu& emu, const char* marker) {
    auto all = emu.transcript();
    DupScan r;
    std::vector<std::pair<std::string,int>> seen;
    for (int y = 0; y < static_cast<int>(all.size()); ++y) {
        const std::string& ln = all[static_cast<std::size_t>(y)];
        if (ln.find(marker) == std::string::npos) continue;
        bool dup = false;
        for (auto& [s, py] : seen)
            if (s == ln) { ++r.count; dup = true; if (r.a < 0) { r.a = py; r.b = y; r.line = ln; } }
        if (!dup) seen.emplace_back(ln, y);
    }
    return r;
}

static void dump(const TermEmu& emu, const char* why) {
    if (!std::getenv("DUMP_TRANSCRIPT")) return;
    auto all = emu.transcript();
    std::fprintf(stderr, "  --- transcript [%s] (%zu sb + %zu view) ---\n",
                 why, emu.scrollback.size(), emu.screen.size());
    for (int y = 0; y < static_cast<int>(all.size()); ++y)
        std::fprintf(stderr, "  %3d|%s%s\n", y,
            y < static_cast<int>(emu.scrollback.size()) ? "" : "V ",
            all[static_cast<std::size_t>(y)].c_str());
}

// The maya public witness for viewport height in a no-tty test context.
using maya::term_rows_for_test;

// ─────────────────────────────────────────────────────────────────────────
// Frame drivers. The Empty→Fresh seed render and the Synced render have
// DIFFERENT signatures (Synced consumes a verify() ShadowWitness), so they
// need separate helpers — exactly the two shapes the runtime uses.
// ─────────────────────────────────────────────────────────────────────────

// Empty→seed→render. Feeds emitted bytes into the emulator; returns the
// Synced arm (asserting we reached it).
static std::optional<InlineFrame<Synced>>
seed_render(TermEmu& emu, int rfd, const Canvas& c, int term_h,
            StylePool& pool, Writer& writer) {
    auto o = InlineFrame<Empty>{}.seed().render(
        c, content_rows(c), term_rows_for_test(term_h), pool, writer,
        /*sync=*/false);
    emu.feed(read_fd(rfd));
    return std::visit([](auto&& a) -> std::optional<InlineFrame<Synced>> {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, InlineFrame<Synced>>) return std::move(a);
        else return std::nullopt;
    }, std::move(o));
}

// (A Synced→verify→render driver would live here for multi-frame
// scenarios; the current single-render / re-emit set doesn't need it. The
// full multi-frame streaming path is covered by the PTY oracle instead —
// see the SCOPE note on scn_oversized_startup.)

// ─────────────────────────────────────────────────────────────────────────
// SCENARIO 1 — OVERSIZED STARTUP places every row exactly once.
//   A first (Fresh) render of a canvas taller than the viewport must put
//   its top (content − term_h) rows into scrollback and its tail in the
//   viewport, every row present EXACTLY once in reading order. This is the
//   append-only base case; capping case (A) or double-emitting would show
//   as a missing row or a duplicate here.
//
//   NOTE ON SCOPE: this harness drives maya's RAW InlineFrame API, which
//   diffs against its own prev_cells shadow but does NOT own the host-side
//   front-trim (dropping the committed prefix from the next canvas) that
//   agentty's view performs between streaming frames. Faithfully modeling
//   incremental multi-frame GROWTH therefore requires replicating that
//   front-trim, which is exactly what the full PTY oracle
//   (tests/scrollback_oracle_test.cpp, driving the real Runtime::render +
//   agentty view) already does. This standalone harness deliberately keeps
//   to the single-render / re-emit invariants it CAN model exactly — the
//   load-bearing corruption class (a committed row re-emitted) — rather
//   than ship scenarios whose "failures" are harness mismodeling, not maya
//   bugs. Honest coverage beats broad-but-wrong coverage.
// ─────────────────────────────────────────────────────────────────────────
static void scn_oversized_startup(int W, int TERM_H) {
    const int CONTENT = TERM_H + 16;
    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    Canvas c = marked_canvas(W, CONTENT, pool, "BOOT");
    auto s = seed_render(emu, rfd, c, TERM_H, pool, writer);
    check(s.has_value(), "startup: oversized first render reaches Synced");
    dump(emu, "startup");

    // Every BOOT-row-k present once, in ascending order, all CONTENT rows.
    auto all = emu.transcript();
    int expected = 0; bool ordered = true;
    for (const auto& ln : all) {
        if (ln.find("BOOT-row-") == std::string::npos) continue;
        if (ln.find("BOOT-row-" + std::to_string(expected)) == std::string::npos) {
            ordered = false; break;
        }
        ++expected;
    }
    check(ordered, "startup: rows appear once each in ascending order");
    check(expected == CONTENT, "startup: all " + std::to_string(CONTENT)
          + " rows present (saw " + std::to_string(expected) + ")");
    auto d = scan_dups(emu, "BOOT-row-");
    check(d.a < 0, "startup: no row duplicated (rows "
                   + std::to_string(d.a) + "/" + std::to_string(d.b) + ")");
    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

// ─────────────────────────────────────────────────────────────────────────
// SCENARIO 2 — committed-prefix IMMUTABILITY at the API level.
//   After an oversized render + commit(overflow), scrollback_prefix_matches
//   must be TRUE for the committed rows against the same canvas (they are
//   byte-identical to what physically overflowed — the runtime's safety
//   gate). And the ScrollbackMarker must clamp: you can never commit more
//   rows than prev_rows. These are the exact predicates Runtime::render's
//   gate relies on; pinning them here catches a regression in the shadow
//   shift / prefix-compare math without needing a full frame loop.
// ─────────────────────────────────────────────────────────────────────────
static void scn_committed_prefix_immutable(int W, int TERM_H) {
    const int CONTENT = TERM_H + 20;
    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);

    Canvas c = marked_canvas(W, CONTENT, pool, "PIN");
    auto s = seed_render(emu, rfd, c, TERM_H, pool, writer);
    check(s.has_value(), "pin: oversized render reaches Synced");
    if (!s) { close(rfd); return; }

    const int overflow = s->rows() - TERM_H;
    check(overflow > 0, "pin: frame overflows the viewport");

    // The committed prefix is byte-identical to the canvas top `overflow`
    // rows — the gate must agree.
    check(s->scrollback_prefix_matches(c, overflow),
          "pin: committed prefix matches canvas top (gate would allow diff)");

    // Over-commit clamps: asking for more than prev_rows yields a marker
    // bounded to prev_rows (never an out-of-range shift).
    auto over = s->scrollback_marker(s->rows() + 100);
    check(over.rows() <= s->rows(),
          "pin: over-commit marker clamps to prev_rows ("
          + std::to_string(over.rows()) + " <= " + std::to_string(s->rows()) + ")");

    // A DIVERGENT canvas top must FAIL the prefix match — the gate must not
    // green-light a diff over a shifted prefix.
    Canvas other = marked_canvas(W, CONTENT, pool, "XXX");  // different labels
    check(!s->scrollback_prefix_matches(other, overflow),
          "pin: divergent prefix correctly FAILS the gate");

    // Capture the marker BEFORE the move-commit consumes `s`.
    auto committed_marker = s->scrollback_marker(overflow);

    // Commit is well-formed: after committing `overflow`, prev_rows drops
    // by exactly that many and stays >= 0.
    const int before = s->rows();
    auto committed = std::move(*s).commit(committed_marker);
    check(committed.rows() == before - overflow,
          "pin: commit drops exactly `overflow` rows ("
          + std::to_string(committed.rows()) + " == "
          + std::to_string(before - overflow) + ")");
    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

// ─────────────────────────────────────────────────────────────────────────
// SCENARIO 3 — RE-EMIT the SAME committed canvas (the classic strand).
//   Rendering an already-committed oversized canvas through a FRESH Empty
//   (empty prefix) MUST strand a duplicate — this is the hazard the runtime
//   avoids by routing recovery through Stale/HardReset, never bare case-A.
//   We assert the hazard IS present (proving the harness detects it) and
//   that the SAFE path (commit + Stale repaint) does NOT strand.
// ─────────────────────────────────────────────────────────────────────────
static void scn_reemit_hazard_and_safe(int W, int TERM_H) {
    const int TALL = TERM_H + 16;

    // (a) HAZARD: bare Empty re-emit strands (harness must DETECT a dup).
    {
        StylePool pool;
        auto [writer, rfd] = make_pipe_writer();
        TermEmu emu(W, TERM_H);
        Canvas c = marked_canvas(W, TALL, pool, "REEM");
        auto s1 = seed_render(emu, rfd, c, TERM_H, pool, writer);
        check(s1.has_value(), "reemit-hazard: first render Synced");
        // Bare Empty re-emit of the SAME canvas (the unsafe shape).
        auto s2 = seed_render(emu, rfd, c, TERM_H, pool, writer);
        check(s2.has_value(), "reemit-hazard: re-emit Synced");
        auto d = scan_dups(emu, "REEM-row-");
        check(d.a >= 0, "reemit-hazard: bare re-emit STRANDS a dup (harness detects corruption)");
        char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
        close(rfd);
    }

    // (b) SAFE: HardReset-shaped re-emit (wipe prefix) does NOT strand.
    {
        StylePool pool;
        auto [writer, rfd] = make_pipe_writer();
        TermEmu emu(W, TERM_H);
        Canvas c = marked_canvas(W, TALL, pool, "SAFE");
        auto s1 = seed_render(emu, rfd, c, TERM_H, pool, writer);
        check(s1.has_value(), "reemit-safe: first render Synced");
        // Model HardReset::render: prepend the \x1b[2J\x1b[3J\x1b[H wipe,
        // then a bare Fresh re-emit. The \x1b[3J clears scrollback so the
        // re-scroll pushes only freshly-blanked rows off — no dup.
        emu.feed(std::string("\x1b[2J\x1b[3J\x1b[H"));
        auto s2 = seed_render(emu, rfd, c, TERM_H, pool, writer);
        check(s2.has_value(), "reemit-safe: wipe re-emit Synced");
        auto d = scan_dups(emu, "SAFE-row-");
        check(d.a < 0, "reemit-safe: HardReset wipe prevents the strand (rows "
                       + std::to_string(d.a) + "/" + std::to_string(d.b) + ")");
        char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
        close(rfd);
    }
}

// ────────────────────────────────────────────────────────────────────────────────────
// SCENARIO 4 — ScrollbackMarker GENERATION binding (type-theoretic SoT).
//   The marker is a capability bound to the IDENTITY of the state that
//   issued it. A marker consumed against the SAME generation commits; a
//   STALE marker (issued before an intervening commit advanced the state)
//   is REJECTED as a no-op — it can never MISAPPLY a superseded row count.
//   This is the deposit-watermark second-accountant class closed by type.
// ────────────────────────────────────────────────────────────────────────────────────
static void scn_marker_generation(int W, int TERM_H) {
    const int TALL = TERM_H + 16;
    StylePool pool;
    auto [writer, rfd] = make_pipe_writer();
    TermEmu emu(W, TERM_H);
    Canvas c = marked_canvas(W, TALL, pool, "GEN");
    auto s = seed_render(emu, rfd, c, TERM_H, pool, writer);
    check(s.has_value(), "gen: oversized render reaches Synced");
    if (!s) { close(rfd); return; }

    const int overflow = s->rows() - TERM_H;
    check(overflow > 0, "gen: frame overflows the viewport");

    // A live state that has rendered carries a non-zero generation.
    const std::uint64_t gen_before = s->state_generation();
    check(gen_before != 0, "gen: rendered state has non-zero generation ('"
          + std::to_string(gen_before) + "')");

    // Mint a marker NOW, at gen_before. This is the stale one we'll try to
    // reuse after an intervening commit.
    auto stale = s->scrollback_marker(overflow);
    check(stale.generation() == gen_before,
          "gen: marker captures the issuing state's generation");

    // (a) LEGITIMATE same-tick commit: a FRESH marker minted from `s` is
    //     consumed immediately — same generation, must succeed & advance.
    const int before = s->rows();
    auto fresh = s->scrollback_marker(overflow);
    auto committed = std::move(*s).commit(fresh);
    check(committed.rows() == before - overflow,
          "gen: same-generation commit succeeds (drops exactly overflow: "
          + std::to_string(committed.rows()) + " == "
          + std::to_string(before - overflow) + ")");

    // The commit advanced the generation — the state's identity changed.
    const std::uint64_t gen_after = committed.state_generation();
    check(gen_after != gen_before,
          "gen: commit advances the generation ("
          + std::to_string(gen_before) + " -> "
          + std::to_string(gen_after) + ")");

    // (b) STALE marker rejection: the `stale` marker (still at gen_before)
    //     applied to the POST-commit state must be a NO-OP — rows unchanged.
    //     Without the generation guard this would drop `overflow` more rows
    //     from a shadow that never pushed them (the deposit-watermark bug).
    const int rows_pre_stale = committed.rows();
    auto after_stale = std::move(committed).commit(stale);
    check(after_stale.rows() == rows_pre_stale,
          "gen: STALE marker is REJECTED as a no-op (rows unchanged: "
          + std::to_string(after_stale.rows()) + " == "
          + std::to_string(rows_pre_stale) + ")");

    char drain[4096]; while (read(rfd, drain, sizeof(drain)) > 0) {}
    close(rfd);
}

int main() {
    std::fprintf(stderr, "scrollback_prefix_harness — append-only-prefix oracle\n");

    // The generation scenario deliberately feeds committed() a STALE
    // marker to prove it is rejected. In a debug-built libmaya that path
    // aborts (the loud tripwire); tell it to no-op instead so the harness
    // can assert the rejection rather than crash. Release libmaya ignores
    // this (already a silent no-op).
    setenv("MAYA_NO_GATE_ABORT", "1", 1);

    // A spread of terminal shapes (narrow/wide, short/tall), so the
    // overflow / commit / prefix-compare arithmetic is exercised across
    // regimes rather than one lucky size.
    struct Shape { int w, h; };
    const Shape shapes[] = {
        { 80, 24 },
        { 40, 10 },
        { 120, 50 },
        { 60, 8  },
    };
    for (auto s : shapes) {
        scn_oversized_startup(s.w, s.h);
        scn_committed_prefix_immutable(s.w, s.h);
        scn_reemit_hazard_and_safe(s.w, s.h);
        scn_marker_generation(s.w, s.h);
    }

    std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::fprintf(stderr, "PASS\n");
    else                 std::fprintf(stderr, "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
