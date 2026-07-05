// scrollback_oracle — drive the REAL maya Runtime::render (app.cpp inline
// path) through a PTY with the REAL agentty view + freeze/trim + tool-card
// lifecycle, and enforce the APPEND-ONLY SCROLLBACK ORACLE:
//
//   At every frame N, the emulator's committed scrollback must be a
//   prefix-extension of scrollback at frame N-1. Any rewrite, reorder,
//   or duplication of an already-committed row is a corruption — no
//   scenario enumeration required.
//
// Additional checks carried over from scrollback_repro.cpp:
//   - unique prose markers ("uniq-T-P") appear at most once in the
//     cumulative transcript (scrollback + viewport),
//   - composer chrome appears at most once.
//
// Scenario coverage beyond the original repro:
//   - tool-card turns: ToolUse Pending -> Running (growing progress_text,
//     live elapsed) -> Done fold, mid-stream, interleaved with prose —
//     the exact shape of the "duplicated prose above tool cards" report.
//   - multiple tool rounds inside one assistant message.
//   - alternating prose-only and tool-heavy turns across enough turns to
//     trigger the frozen front-trim (commit_scrollback) repeatedly.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <clocale>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <maya/app/app.hpp>
#include <maya/core/anim_clock.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolUse;

static FILE* err = nullptr;
static int g_failures = 0;

// ── Minimal ANSI terminal emulator with native scrollback ──────────────
//
// Width model: cells are decoded UTF-8 CODEPOINTS, and each codepoint
// occupies wcwidth(3) columns — an independent width reference (libc's
// table, NOT maya's), so a maya-side width bug shows up as drift here
// instead of being self-consistently invisible. Non-ASCII codepoints
// render as '?' in the transcript (marker checks are ASCII), but their
// column footprint is faithful.
struct TermEmu {
    int cols, rows;
    int cx = 0, cy = 0;
    std::vector<std::string> screen;
    std::vector<std::string> scrollback;
    bool decawm = true;

    TermEmu(int c, int r) : cols(c), rows(r) {
        screen.assign((std::size_t)rows, std::string((std::size_t)cols, ' '));
    }
    std::string& line(int y) { return screen[(std::size_t)y]; }

    // Place a decoded codepoint of display width w (1 or 2).
    void put_cp(char32_t cp, int w) {
        if (w <= 0) return;                    // combining / zero-width: skip
        if (cx + w > cols) {
            if (decawm) { cx = 0; cursor_newline(); }
            else        { cx = cols - w; if (cx < 0) cx = 0; }
        }
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        std::string& l = line(cy);
        if ((int)l.size() < cols) l.resize((std::size_t)cols, ' ');
        const char vis = (cp < 128) ? (char)cp : '?';
        l[(std::size_t)cx] = vis;
        for (int k = 1; k < w && cx + k < cols; ++k)
            l[(std::size_t)(cx + k)] = (cp < 128) ? ' ' : '?';
        cx += w;
    }
    void cursor_newline() {
        if (cy < rows - 1) { ++cy; return; }
        std::string top = screen.front();
        while (!top.empty() && top.back() == ' ') top.pop_back();
        scrollback.push_back(std::move(top));
        screen.erase(screen.begin());
        screen.push_back(std::string((std::size_t)cols, ' '));
    }
    void carriage_return() { cx = 0; }
    void cursor_up(int n)   { cy -= n; if (cy < 0) cy = 0; }
    void cursor_down(int n) { for (int i = 0; i < n; ++i) cursor_newline(); }
    void cursor_fwd(int n)  { cx += n; if (cx > cols) cx = cols; }
    void erase_to_eol() {
        std::string& l = line(cy);
        if ((int)l.size() < cols) l.resize((std::size_t)cols, ' ');
        for (int x = cx; x < cols; ++x) l[(std::size_t)x] = ' ';
    }
    void erase_line_from_start() {
        std::string& l = line(cy);
        if ((int)l.size() < cols) l.resize((std::size_t)cols, ' ');
        for (int x = 0; x <= cx && x < cols; ++x) l[(std::size_t)x] = ' ';
    }
    void erase_line_all() {
        line(cy).assign((std::size_t)cols, ' ');
    }
    void erase_to_eos() {
        erase_to_eol();
        for (int y = cy + 1; y < rows; ++y)
            line(y).assign((std::size_t)cols, ' ');
    }
    bool saw_3j = false;   // frame-scoped: CSI 3J observed
    bool saw_2j = false;   // frame-scoped: CSI 2J observed
    std::string carry;   // incomplete trailing ESC/UTF-8 sequence from last feed

    void feed(const std::string& chunk) {
        // A pty read can split an escape sequence or a UTF-8 codepoint
        // across two reads (finite pty buffer + maya's write_or_buffer
        // flushing the remainder on a later frame). Prepend the stash and
        // re-stash any incomplete tail rather than printing it as text.
        std::string b = std::move(carry);
        carry.clear();
        b += chunk;
        std::size_t i = 0, n = b.size();
        while (i < n) {
            unsigned char ch = (unsigned char)b[i];
            if (ch == '\r') { carriage_return(); ++i; continue; }
            if (ch == '\n') { cursor_newline(); ++i; continue; }
            if (ch == 0x1b) {
                std::size_t adv = parse_esc(b, i);
                if (adv == kIncomplete) { carry = b.substr(i); return; }
                i = adv; continue;
            }
            if (ch < 0x20) { ++i; continue; }
            // Decode one UTF-8 codepoint.
            char32_t cp = 0; int len = 1;
            if      (ch < 0x80)      { cp = ch; len = 1; }
            else if ((ch >> 5) == 0x6)  len = 2;
            else if ((ch >> 4) == 0xe)  len = 3;
            else if ((ch >> 3) == 0x1e) len = 4;
            else { ++i; continue; }               // stray continuation byte
            if (len > 1) {
                if (i + (std::size_t)len > n) { carry = b.substr(i); return; }
                cp = (char32_t)(ch & (0xff >> (len + 1)));
                for (int k = 1; k < len; ++k)
                    cp = (cp << 6) | (char32_t)((unsigned char)b[i+(std::size_t)k] & 0x3f);
            }
            i += (std::size_t)len;
            int w = (cp < 128) ? 1 : wcwidth((wchar_t)cp);
            if (w < 0) w = 1;                     // unknown: assume 1 like most emulators
            put_cp(cp, w);
        }
    }
    static constexpr std::size_t kIncomplete = (std::size_t)-1;
    std::size_t parse_esc(const std::string& b, std::size_t i) {
        const std::size_t n = b.size();
        if (i + 1 >= n) return kIncomplete;
        if (b[i + 1] == ']') {           // OSC — skip to BEL or ST
            std::size_t j = i + 2;
            while (j < n && b[j] != '\x07'
                   && !(b[j] == '\x1b' && j + 1 < n && b[j + 1] == '\\')) ++j;
            if (j < n && b[j] == '\x07') return j + 1;
            if (j + 1 < n) return j + 2;
            return kIncomplete;          // OSC still open at buffer end
        }
        if (b[i + 1] != '[') return i + 2;
        std::size_t j = i + 2;
        bool priv = false;
        if (j < n && b[j] == '?') { priv = true; ++j; }
        int params[4] = {0, 0, 0, 0};
        bool have[4] = {false, false, false, false};
        int pi = 0;
        while (j < n && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
            if (b[j] == ';') { if (pi < 3) ++pi; ++j; continue; }
            params[pi] = params[pi] * 10 + (b[j] - '0');
            have[pi] = true; ++j;
        }
        if (j >= n) return kIncomplete;   // CSI still open at buffer end
        char fin = b[j];
        const int p = have[0] ? params[0] : 0;
        switch (fin) {
            case 'A': cursor_up(p ? p : 1); break;
            case 'B': cursor_down(p ? p : 1); break;
            case 'C': cursor_fwd(p ? p : 1); break;
            case 'D': cx -= (p ? p : 1); if (cx < 0) cx = 0; break;
            case 'G': cx = (p ? p - 1 : 0); if (cx < 0) cx = 0; break;
            case 'H': case 'f': {
                // CUP row;col (1-based); missing params default to 1.
                int r = have[0] && params[0] ? params[0] : 1;
                int c = have[1] && params[1] ? params[1] : 1;
                cy = r - 1; if (cy < 0) cy = 0; if (cy >= rows) cy = rows - 1;
                cx = c - 1; if (cx < 0) cx = 0; if (cx > cols) cx = cols;
                break;
            }
            case 'K':
                if (p == 1)      erase_line_from_start();
                else if (p == 2) erase_line_all();
                else             erase_to_eol();
                break;
            case 'J':
                if (p == 2 || p == 3) {
                    if (p == 3) { scrollback.clear(); saw_3j = true; }
                    saw_2j = true;
                    for (auto& l : screen) l.assign((std::size_t)cols, ' ');
                } else {
                    erase_to_eos();
                }
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

static std::string read_all(int fd) {
    std::string s;
    char buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { s.append(buf, (std::size_t)n); continue; }
        break;
    }
    return s;
}

// Advance the ANIMATION clock (maya::anim_now_ms — the one time source
// every time-driven widget reads: reveal cursor, scramble window, finalize
// ramp, anim::Clock/Motion) WITHOUT sleeping. The widgets compute exactly
// the ages/phases/deadlines they would after a real sleep, so the scenario
// exercises identical animation dynamics at ~zero wall cost — the suite
// used to spend >95% of its runtime in sleep_for.
static void tick(int ms = 20) {
    maya::testing::advance_anim_clock_ms(ms);
}

// ── Oracle state (per shape) ────────────────────────────────────────────
struct Oracle {
    std::vector<std::string> committed;   // scrollback as of last frame
    std::vector<std::string> committed_at; // frame tag that committed each row
    int frames_checked = 0;
};

static void dump_transcript(const TermEmu& emu) {
    auto all = emu.transcript();
    while (!all.empty() && all.back().empty()) all.pop_back();
    std::fprintf(err, "  --- transcript (%zu sb + %d view) ---\n",
                 emu.scrollback.size(), emu.rows);
    for (int y = 0; y < (int)all.size(); ++y)
        std::fprintf(err, "  %4d|%s%s\n", y,
            y < (int)emu.scrollback.size() ? "" : "V ",
            all[(std::size_t)y].c_str());
}

// The append-only oracle. Returns true on failure.
static bool check_oracle(Oracle& orc, const TermEmu& emu,
                         const std::string& tag) {
    const auto& sb = emu.scrollback;
    ++orc.frames_checked;
    bool failed = false;

    if (sb.size() < orc.committed.size()) {
        ++g_failures; failed = true;
        std::fprintf(err,
            "  FAIL[%s][oracle]: scrollback SHRANK %zu -> %zu "
            "(committed rows vanished)\n",
            tag.c_str(), orc.committed.size(), sb.size());
    } else {
        for (std::size_t i = 0; i < orc.committed.size(); ++i) {
            if (sb[i] != orc.committed[i]) {
                ++g_failures; failed = true;
                std::fprintf(err,
                    "  FAIL[%s][oracle]: committed row %zu REWRITTEN\n"
                    "    was: '%s'  (committed at frame %s)\n"
                    "    now: '%s'\n",
                    tag.c_str(), i, orc.committed[i].c_str(),
                    i < orc.committed_at.size() ? orc.committed_at[i].c_str() : "?",
                    sb[i].c_str());
                break;
            }
        }
    }
    for (std::size_t i = orc.committed.size(); i < sb.size(); ++i)
        orc.committed_at.push_back(tag);
    orc.committed_at.resize(sb.size(), tag);
    orc.committed = sb;
    return failed;
}

// Marker-uniqueness + composer-chrome checks over cumulative transcript.
static bool check_transcript(TermEmu& emu, const std::string& tag,
                             bool dump_on_fail) {
    auto all = emu.transcript();
    while (!all.empty() && all.back().empty()) all.pop_back();

    bool failed = false;
    std::vector<std::pair<std::string,int>> seen;
    for (int y = 0; y < (int)all.size(); ++y) {
        const std::string& ln = all[(std::size_t)y];
        auto pos = ln.find("uniq-");
        if (pos == std::string::npos) continue;
        std::size_t e = pos;
        while (e < ln.size() && ln[e] != ' ' && ln[e] != '.') ++e;
        std::string tok = ln.substr(pos, e - pos);
        for (auto& [s, py] : seen) {
            if (s == tok) {
                ++g_failures;
                failed = true;
                std::fprintf(err,
                    "  FAIL[%s]: marker %s duplicated at transcript rows %d and %d "
                    "(scrollback=%zu)\n",
                    tag.c_str(), tok.c_str(), py, y, emu.scrollback.size());
                break;
            }
        }
        if (failed) break;
        seen.emplace_back(std::move(tok), y);
    }

    int composer_hits = 0;
    for (const auto& ln : all)
        if (ln.find("type a message") != std::string::npos
            || ln.find("type to queue") != std::string::npos)
            ++composer_hits;
    if (composer_hits > 1) {
        ++g_failures;
        failed = true;
        std::fprintf(err, "  FAIL[%s]: composer chrome appears %d times in transcript\n",
                     tag.c_str(), composer_hits);
    }

    if (failed && dump_on_fail) dump_transcript(emu);
    return failed;
}

// ── Turn scripts ────────────────────────────────────────────────────────
//
// Two turn shapes, alternated:
//   prose turn  — stream tall markdown (the original repro's shape).
//   tool turn   — stream a short prose lead-in, then run kRounds tool
//                 cards through Pending -> Running(progress grows) ->
//                 Done(fold), each followed by more prose. Mirrors the
//                 reducer's mutations (with_live_tool-style: mutate the
//                 live tail in place, bump nothing else).

struct Ctx {
    maya::detail::Runtime* rt;
    TermEmu*   emu;
    Oracle*    orc;
    int        master;
    Model*     m;
    bool       done = false;

    bool frame(const std::string& tag) {
        emu->saw_3j = emu->saw_2j = false;
        const std::size_t sb_before = emu->scrollback.size();
        const int rows_before = rt->inline_content_rows();
        static const bool trace = std::getenv("ORACLE_TRACE") != nullptr;
        // Snapshot the committed scrollback tail BEFORE this render, so if
        // this frame drives maya non-Synced we can print the exact rows
        // that were committed and about to be rewritten.
        std::vector<std::string> sb_snapshot = emu->scrollback;
        (void)rt->render(agentty::ui::view(*m));
        const int rows_after = rt->inline_content_rows();
        if (trace)
            std::fprintf(err, "  [trace %s] rows %d -> %d sb %zu\n",
                         tag.c_str(), rows_before, rows_after,
                         emu->scrollback.size());
        if (const char* want = std::getenv("ORACLE_DUMP_AT");
            want && tag == want) {
            std::fprintf(err, "  [dump %s] viewport:\n", tag.c_str());
            for (std::size_t i = 0; i < emu->screen.size(); ++i)
                std::fprintf(err, "    vp%3zu|%s\n", i,
                             emu->screen[i].c_str());
            const std::size_t lo = emu->scrollback.size() > 12
                                 ? emu->scrollback.size() - 12 : 0;
            for (std::size_t i = lo; i < emu->scrollback.size(); ++i)
                std::fprintf(err, "    sb%4zu|%s\n", i,
                             emu->scrollback[i].c_str());
        }
        if (rows_before != 0 && rows_after == 0) {
            std::fprintf(err, "  [dbg] frame %s: maya entered non-Synced (rows %d -> 0)\n",
                         tag.c_str(), rows_before);
            const std::size_t lo = sb_snapshot.size() > 40 ? sb_snapshot.size() - 40 : 0;
            std::fprintf(err, "  [dbg] committed rows [%zu..%zu) BEFORE this render:\n",
                         lo, sb_snapshot.size());
            for (std::size_t i = lo; i < sb_snapshot.size(); ++i)
                std::fprintf(err, "    sb%4zu|%s\n", i, sb_snapshot[i].c_str());
        }
        emu->feed(read_all(master));
        if (emu->saw_3j || emu->saw_2j)
            std::fprintf(err, "  [frame %s] %s%s sb %zu -> %zu\n",
                         tag.c_str(),
                         emu->saw_2j ? "2J " : "", emu->saw_3j ? "3J" : "",
                         sb_before, emu->scrollback.size());
        bool f = check_oracle(*orc, *emu, tag);
        if (f) dump_transcript(*emu);
        f |= check_transcript(*emu, tag, /*dump_on_fail=*/true);
        return f;
    }
};

// Stream `paras` markdown paragraphs into the live assistant message.
// `wrapping=true` makes each paragraph long enough to wrap 3-4 rows at
// 80 cols — the screenshot's duplicated prose was a WRAPPED paragraph
// whose first duplicate ended mid-word (a mid-reveal snapshot), so
// wrap + reveal interaction is a required axis.
// `burst>1` renders only every burst-th mutation — models the real app
// skipping frames under load (visual_hash gate / slow terminal), where
// several deltas land in ONE rendered frame.
static bool stream_paras(Ctx& cx, Message& msg, const std::string& st,
                         int first, int paras, const char* tag_prefix,
                         bool wrapping = false, int burst = 1) {
    int since_render = 0;
    for (int d = first; d < first + paras; ++d) {
        msg.streaming_text +=
            "\n\nParagraph uniq-" + st + "-" + std::to_string(d)
            + " covers stage " + std::to_string(d)
            + " of the pipeline in turn " + st + ".";
        if (wrapping)
            msg.streaming_text +=
                " It elaborates at considerable length about the provider"
                " objects and whichever translation unit defines the missing"
                " symbol so the paragraph wraps across several terminal rows"
                " exactly like real assistant prose does in the field.";
        if (++since_render >= burst) {
            since_render = 0;
            tick(); if (cx.frame(std::string(tag_prefix) + "-d" + std::to_string(d))) return true;
            tick(); if (cx.frame(std::string(tag_prefix) + "-a" + std::to_string(d))) return true;
        }
    }
    if (since_render) {
        tick(); if (cx.frame(std::string(tag_prefix) + "-flush")) return true;
    }
    return false;
}

// Settle current assistant message through the production reveal gate,
// then freeze + trim exactly as meta.cpp Tick / execute_cmd do.
static bool settle_freeze_trim(Ctx& cx, const std::string& st) {
    Model& m = *cx.m;
    // Mirror production (meta.cpp Tick) EXACTLY:
    //   1. finalize_turn moves streaming_text → text and arms the reveal
    //      finalize ramp (pending_settle_freeze), but does NOT finish()
    //      the widget — the typewriter keeps gliding to the live edge.
    //   2. Subsequent Ticks render the draining reveal; freeze fires ONLY
    //      once live_tail_reveal_settled() is true (widget flipped live_
    //      off on its own), so the last LIVE frame maya cached already IS
    //      the settled shape and settle_message_md()'s finish() is a
    //      byte-idempotent no-op.
    // Calling settle_message_md() up front (the old harness bug) forced an
    // immediate finish() BEFORE the reveal drained — changing a committed
    // row and driving maya into a HardReset at the settle frame.
    for (std::size_t i = m.ui.frozen_through;
         i < m.d.current.messages.size(); ++i) {
        auto& b = m.d.current.messages[i];
        if (b.role != Role::Assistant) continue;
        if (!b.streaming_text.empty()) {
            b.text += b.streaming_text;
            b.streaming_text.clear();
        }
    }
    m.s.phase = agentty::phase::Idle{};
    int guard = 0;
    do {
        tick();
        if (cx.frame("t" + st + "-settle" + std::to_string(guard))) return true;
    } while (!agentty::app::detail::live_tail_reveal_settled(m)
             && ++guard < 200);

    // Reveal drained — NOW settle each message's markdown (byte-idempotent
    // finish) and freeze, exactly as the Tick's live_tail_reveal_settled
    // branch does.
    for (std::size_t i = m.ui.frozen_through;
         i < m.d.current.messages.size(); ++i) {
        auto& b = m.d.current.messages[i];
        if (b.role != Role::Assistant || b.text.empty()) continue;
        agentty::app::detail::settle_message_md(m, b);
    }
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    std::fprintf(err, "  [dbg] t%s pre-trim: maya_rows=%d frozen_row_total=%zu frozen_entries=%zu\n",
                 st.c_str(), cx.rt->inline_content_rows(),
                 (std::size_t)m.ui.frozen.row_total(), m.ui.frozen.size());
    auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
    using Cmd = maya::Cmd<agentty::Msg>;
    if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner)) {
        std::fprintf(err, "  [info] turn %s: trim commit_scrollback(%d) -> frozen_row_total=%zu\n",
                     st.c_str(), c->rows, (std::size_t)m.ui.frozen.row_total());
        cx.rt->commit_inline_prefix(c->rows);
        // NOTE: after a prefix commit maya's shadow shifted; the oracle's
        // committed snapshot is still valid (commit emits zero bytes).
    }
    if (cx.frame("t" + st + "-freeze")) return true;
    for (int f = 0; f < 6; ++f) {
        tick();
        if (cx.frame("t" + st + "-idle" + std::to_string(f))) return true;
    }
    return false;
}

// A prose-only turn (original repro shape).
static bool prose_turn(Ctx& cx, int t, int H) {
    Model& m = *cx.m;
    const std::string st = std::to_string(t);

    { // submit
        Message u; u.role = Role::User;
        u.text = "turn " + st + ": please explain everything again";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        if (cx.frame("t" + st + "-submit")) return true;
    }

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Turn " + st + " opening: uniq-" + st + "-open.";
    m.d.current.messages.push_back(std::move(a));
    for (int f = 0; f < 3; ++f) { tick(); if (cx.frame("t" + st + "-o" + std::to_string(f))) return true; }

    if (stream_paras(cx, m.d.current.messages.back(), st, 0, 2 * H / 3, ("t" + st).c_str()))
        return true;

    return settle_freeze_trim(cx, st);
}

// A tool-card turn: prose lead-in, then rounds of
// tool Pending -> Running (progress grows) -> Done, prose between rounds.
// Hostile by design:
//   - enough WRAPPING prose before the first card that the live tail
//     already overflows the viewport when the card starts growing,
//   - Running progress grows by MANY lines (long bash job shape, 30 KB
//     output in the field report),
//   - burst frames — mutations outnumber rendered frames.
static bool tool_turn(Ctx& cx, int t, int H) {
    Model& m = *cx.m;
    const std::string st = std::to_string(t);

    { // submit — mirror submit_message: freeze prior turn, then trim
        Message u; u.role = Role::User;
        u.text = "turn " + st + ": run the checks and explain uniq-" + st + "-ask.";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner)) {
            std::fprintf(err, "  [info] turn %s: submit trim commit_scrollback(%d)\n",
                         st.c_str(), c->rows);
            cx.rt->commit_inline_prefix(c->rows);
        }
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        if (cx.frame("t" + st + "-submit")) return true;
    }

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Turn " + st + " tool opening: uniq-" + st + "-open.";
    m.d.current.messages.push_back(std::move(a));
    for (int f = 0; f < 2; ++f) { tick(); if (cx.frame("t" + st + "-o" + std::to_string(f))) return true; }

    // Wrapping prose lead-in: enough to overflow the viewport BEFORE the
    // first card exists, so every later card grow happens while the tail
    // is taller than the terminal.
    const int lead = H / 4 + 3;
    if (stream_paras(cx, m.d.current.messages.back(), st, 0, lead,
                     ("t" + st + "-pre").c_str(), /*wrapping=*/true, /*burst=*/2))
        return true;

    const int kRounds = 3;
    int para_next = lead;
    for (int r = 0; r < kRounds; ++r) {
        auto& msg = m.d.current.messages.back();
        const std::string rt_tag = "t" + st + "-r" + std::to_string(r);

        // Round 2 is the PARALLEL-TOOL round: TWO tools on ONE message
        // (the model emitting parallel tool_use blocks — a real and
        // common wire shape). Tool A is quick; tool B's Running progress
        // grows past 2× the viewport, committing the panel's TITLE and
        // A's event-header row into native scrollback while BOTH are
        // still live chrome. Then A flips Done on the SAME frame that
        // B's progress grows — a GROW frame carrying a committed-row
        // mutation (title counter 0/2→1/2, A's spinner→✓). The gate's
        // grow arm HardResets on mismatch: the destructive wipe class.
        if (r == 2) {
            for (int tno = 0; tno < 2; ++tno) {
                ToolUse tc;
                tc.id   = agentty::ToolCallId{"tool-" + st + "-" + std::to_string(r)
                                              + "-" + std::to_string(tno)};
                tc.name = agentty::ToolName{"Bash"};
                tc.args = nlohmann::json{
                    {"command", "make par-" + std::to_string(tno)},
                    {"display_description",
                     "uniq-" + st + "c" + std::to_string(r) + (tno ? "b" : "a") + ". go"}
                };
                tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                msg.tool_calls.push_back(std::move(tc));
            }
            m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
            tick(); if (cx.frame(rt_tag + "-pending2")) return true;

            auto& ta = msg.tool_calls[msg.tool_calls.size() - 2];
            auto& tb = msg.tool_calls.back();
            ta.status = ToolUse::Running{std::chrono::steady_clock::now(), ""};
            tb.status = ToolUse::Running{std::chrono::steady_clock::now(), ""};
            tick(); if (cx.frame(rt_tag + "-run2both")) return true;

            // B grows past 2× viewport — commits title + A's header row.
            const int kProg = 2 * H + 10;
            for (int p = 0; p < kProg; ++p) {
                auto& run = std::get<ToolUse::Running>(tb.status);
                run.progress_text +=
                    "[par " + std::to_string(p) + "/" + std::to_string(kProg)
                    + "] linking shard " + std::to_string(p) + "...\n";
                if (p % 3 == 2) {
                    tick(); if (cx.frame(rt_tag + "-bgrow" + std::to_string(p + 1))) return true;
                }
            }

            // A flips Done AND B grows more — ONE frame, net GROW, with
            // the committed title counter + A's glyph both mutating.
            {
                auto sa = std::get<ToolUse::Running>(ta.status).started_at;
                ta.status = ToolUse::Done{
                    sa, std::chrono::steady_clock::now(), "ok: A done"};
                ta.expanded = false;
                auto& run = std::get<ToolUse::Running>(tb.status);
                for (int k = 0; k < 4; ++k)
                    run.progress_text += "[par tail " + std::to_string(k) + "]\n";
                tick(); if (cx.frame(rt_tag + "-aflip")) return true;
            }

            // B Done; then continuation prose (new message, below panel).
            {
                auto sb = std::get<ToolUse::Running>(tb.status).started_at;
                tb.status = ToolUse::Done{
                    sb, std::chrono::steady_clock::now(), "ok: B done"};
                tb.expanded = false;
                tick(); if (cx.frame(rt_tag + "-bdone")) return true;
            }
            {
                Message cont; cont.role = Role::Assistant;
                m.d.current.messages.push_back(std::move(cont));
            }
            m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
            if (stream_paras(cx, m.d.current.messages.back(), st, para_next, 3,
                             (rt_tag + "-post").c_str(), /*wrapping=*/true, /*burst=*/2))
                return true;
            para_next += 3;
            continue;
        }

        // Tool appears: Pending, args streaming in.
        {
            ToolUse tc;
            tc.id   = agentty::ToolCallId{"tool-" + st + "-" + std::to_string(r)};
            tc.name = agentty::ToolName{"Bash"};
            tc.args = nlohmann::json{
                {"command", "make check-round-" + std::to_string(r)},
                // Keep the unique token SHORT and EARLY: narrow shapes
                // (46 cols) clip long card descriptions with an ellipsis,
                // which would truncate two distinct markers to the same
                // prefix and false-positive the uniqueness check.
                {"display_description", "uniq-" + st + "c" + std::to_string(r) + ". go"}
            };
            tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
            m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
            msg.tool_calls.push_back(std::move(tc));
            tick(); if (cx.frame(rt_tag + "-pending")) return true;
        }

        // Running: progress_text grows a LOT, in bursts — the long bash
        // job whose card keeps growing while the tail is overflowed.
        // Round 1 is the CHROME-COMMIT round: progress grows past 2× the
        // viewport so the panel's TITLE + event-header rows physically
        // scroll into native scrollback while the tool is still Running
        // (spinner glyph, live title). Any later byte change to those
        // rows (counter tick, running→done glyph, title_end swap) is a
        // committed-row rewrite — the user's "stacked ACTIONS fragments"
        // screenshot class.
        {
            auto& tc = msg.tool_calls.back();
            tc.status = ToolUse::Running{std::chrono::steady_clock::now(), ""};
            tick(); if (cx.frame(rt_tag + "-run0")) return true;
            const int kProgress = (r == 1) ? (2 * H + 10) : (H / 2 + 8);
            for (int p = 0; p < kProgress; ++p) {
                auto& run = std::get<ToolUse::Running>(tc.status);
                run.progress_text +=
                    "[" + std::to_string(p) + "/" + std::to_string(kProgress)
                    + "] compiling translation unit " + std::to_string(p)
                    + " of round " + std::to_string(r) + "...\n";
                if (p % 3 == 2) {   // burst: 3 growth steps per rendered frame
                    tick(); if (cx.frame(rt_tag + "-run" + std::to_string(p + 1))) return true;
                }
            }
        }

        // Done: card folds to its settled height (the big SHRINK).
        // Round 1: flip Done AND land the continuation's first prose
        // bytes BEFORE the next render — one frame carries both the
        // committed-chrome mutation (spinner→✓, title counter, footer)
        // and a GROW (new prose rows below). In production this is a
        // burst frame: ToolExecOutput + the continuation stream's first
        // deltas land between two renders. Grow+mismatch is the
        // HardReset arm of maya's gate — the destructive recovery.
        {
            auto& tc = msg.tool_calls.back();
            auto started = std::get<ToolUse::Running>(tc.status).started_at;
            tc.status = ToolUse::Done{
                started, std::chrono::steady_clock::now(),
                "ok: 42 tests passed (round " + std::to_string(r) + ")"};
            tc.expanded = false;
            if (r != 1) {
                tick(); if (cx.frame(rt_tag + "-done")) return true;
                tick(); if (cx.frame(rt_tag + "-fold")) return true;
            }
            // r == 1: NO render here — the Done flip rides the same
            // frame as the continuation prose below.
        }

        // More WRAPPING prose after the card. In PRODUCTION this is a
        // NEW sub-turn: kick_pending_tools pushes a fresh Assistant
        // placeholder after the tools execute, and the continuation
        // stream's text deltas land on THAT message. So the post-tool
        // prose renders in its OWN text slot BELOW the panel (in
        // turn_config_for_assistant_run's text(i)→panel(i)→text(i+1)
        // order), never concatenated into the pre-tool message's
        // streaming_text above the panel. Appending it to the same
        // message (the old harness bug) grew the markdown block ABOVE
        // the panel and pushed the panel's already-committed title row
        // down — an off-viewport rewrite that never occurs on the wire.
        {
            Message cont; cont.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(cont));
        }
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        if (stream_paras(cx, m.d.current.messages.back(), st, para_next, 3,
                         (rt_tag + "-post").c_str(), /*wrapping=*/true, /*burst=*/2))
            return true;
        para_next += 3;
    }

    return settle_freeze_trim(cx, st);
}

// A WRITE/EDIT turn: the mutating-tool shape. Distinct hazard from the
// Bash rounds: a streaming write/edit card renders a SMALL tail-windowed
// preview (show_all=false, ~code_tail rows) while its args grow, then at
// terminal flips to the FULL body (show_all=true) — a large height GROW
// in one frame while committed prose rows sit above the panel. The edit
// additionally switches RENDER PATHS at settle: streaming EditDiff-from-
// args → fence-parsed GitDiff from the tool output. Both transitions
// must be pure bottom-appends w.r.t. committed rows.
static bool write_edit_turn(Ctx& cx, int t, int H) {
    Model& m = *cx.m;
    const std::string st = std::to_string(t);

    { // submit — mirror submit_message: freeze prior turn, then trim
        Message u; u.role = Role::User;
        u.text = "turn " + st + ": write the file and fix uniq-" + st + "-ask.";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner)) {
            std::fprintf(err, "  [info] turn %s: submit trim commit_scrollback(%d)\n",
                         st.c_str(), c->rows);
            cx.rt->commit_inline_prefix(c->rows);
        }
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        if (cx.frame("t" + st + "-submit")) return true;
    }

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Turn " + st + " write opening: uniq-" + st + "-open.";
    m.d.current.messages.push_back(std::move(a));
    for (int f = 0; f < 2; ++f) { tick(); if (cx.frame("t" + st + "-o" + std::to_string(f))) return true; }

    // Wrapping prose lead-in so the tail already overflows the viewport
    // before the card exists — every later card mutation happens while
    // rows above the card are committed.
    const int lead = H / 4 + 3;
    if (stream_paras(cx, m.d.current.messages.back(), st, 0, lead,
                     ("t" + st + "-pre").c_str(), /*wrapping=*/true, /*burst=*/2))
        return true;

    // ── WRITE: args stream in (content grows to ~3× viewport), card is
    //    Pending with a tail-window preview; then Running; then Done →
    //    the card expands tail-window → FULL body in one frame.
    {
        auto& msg = m.d.current.messages.back();
        ToolUse tc;
        tc.id   = agentty::ToolCallId{"write-" + st};
        tc.name = agentty::ToolName{"write"};
        tc.args = nlohmann::json{{"file_path", "src/gen_" + st + ".cpp"},
                                 {"content", ""}};
        tc.args_streaming = "{\"file_path\":\"src/gen_" + st + ".cpp\",\"content\":\"";
        tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
        msg.tool_calls.push_back(std::move(tc));
        tick(); if (cx.frame("t" + st + "-w-pending")) return true;

        // Stream the content: the reducer fills args["content"] from the
        // partial-JSON decoder as input_json deltas land; args_streaming
        // grows in lockstep (it feeds compute_render_key so caches can't
        // stale-blit). 3×H lines, bursts of 5 lines, render every 2nd.
        const int kLines = 3 * H;
        std::string content;
        int since = 0;
        for (int ln = 0; ln < kLines; ++ln) {
            std::string line = "    auto v" + std::to_string(ln)
                + " = compute_" + st + "(" + std::to_string(ln) + ");\n";
            content += line;
            auto& wt = msg.tool_calls.back();
            wt.args_streaming += line;
            if (ln % 5 == 4) {
                wt.args["content"] = content;   // decoder snapshot cadence
                if (++since >= 2) {
                    since = 0;
                    tick(); if (cx.frame("t" + st + "-w-grow" + std::to_string(ln + 1))) return true;
                }
            }
        }
        {
            auto& wt = msg.tool_calls.back();
            wt.args["content"] = content;
            tick(); if (cx.frame("t" + st + "-w-args-done")) return true;

            // Execute: Running (brief), then Done. The Done flip switches
            // the body tail-window → show_all FULL content (~3×H rows) —
            // the big one-frame GROW this turn exists to check.
            m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
            wt.status = ToolUse::Running{std::chrono::steady_clock::now(), ""};
            tick(); if (cx.frame("t" + st + "-w-run")) return true;
            auto started = std::get<ToolUse::Running>(wt.status).started_at;
            wt.status = ToolUse::Done{
                started, std::chrono::steady_clock::now(),
                "Wrote " + std::to_string(content.size()) + " bytes to src/gen_"
                    + st + ".cpp"};
            wt.expanded = false;
            tick(); if (cx.frame("t" + st + "-w-done")) return true;
            tick(); if (cx.frame("t" + st + "-w-fold")) return true;
        }
    }

    // Continuation prose on a NEW message (production sub-turn shape).
    {
        Message cont; cont.role = Role::Assistant;
        m.d.current.messages.push_back(std::move(cont));
    }
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    if (stream_paras(cx, m.d.current.messages.back(), st, lead, 3,
                     ("t" + st + "-w-post").c_str(), /*wrapping=*/true, /*burst=*/2))
        return true;

    // ── EDIT: hunks stream into args.edits one by one (elided per-hunk
    //    preview), then Done with a ```diff fence in the output — the
    //    settled card re-renders through the fence-parse GitDiff path in
    //    FULL (show_all), a render-path switch + height grow in one frame.
    {
        auto& msg = m.d.current.messages.back();
        ToolUse tc;
        tc.id   = agentty::ToolCallId{"edit-" + st};
        tc.name = agentty::ToolName{"edit"};
        tc.args = nlohmann::json{{"path", "src/gen_" + st + ".cpp"},
                                 {"edits", nlohmann::json::array()}};
        tc.args_streaming = "{\"path\":\"src/gen_" + st + ".cpp\",\"edits\":[";
        tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
        msg.tool_calls.push_back(std::move(tc));
        m.s.phase = agentty::phase::ExecutingTool{agentty::phase::Active{}};
        tick(); if (cx.frame("t" + st + "-e-pending")) return true;

        // 5 hunks, each ~6 lines per side, landing one per rendered frame.
        const int kHunks = 5;
        std::string diff_body;
        for (int hk = 0; hk < kHunks; ++hk) {
            std::string ot, nt;
            for (int l = 0; l < 6; ++l) {
                ot += "old_" + st + "_h" + std::to_string(hk) + "_l"
                    + std::to_string(l) + "();\n";
                nt += "new_" + st + "_h" + std::to_string(hk) + "_l"
                    + std::to_string(l) + "();\n";
            }
            auto& et = msg.tool_calls.back();
            et.args["edits"].push_back({{"old_text", ot}, {"new_text", nt}});
            et.args_streaming += ot + nt;   // size proxy for render key
            diff_body += "@@ -" + std::to_string(hk * 10 + 1) + ",6 +"
                       + std::to_string(hk * 10 + 1) + ",6 @@\n";
            for (int l = 0; l < 6; ++l)
                diff_body += "-old_" + st + "_h" + std::to_string(hk) + "_l"
                           + std::to_string(l) + "();\n";
            for (int l = 0; l < 6; ++l)
                diff_body += "+new_" + st + "_h" + std::to_string(hk) + "_l"
                           + std::to_string(l) + "();\n";
            tick(); if (cx.frame("t" + st + "-e-hunk" + std::to_string(hk))) return true;
        }

        // Done: output carries the ```diff fence → settled GitDiff path,
        // full body. Land the flip and the continuation's first prose
        // bytes in ONE frame (production burst: ToolExecOutput + first
        // continuation deltas between two renders) — grow + body-path
        // switch on the same frame is the HardReset arm if anything above
        // the seam changed.
        {
            auto& et = msg.tool_calls.back();
            et.status = ToolUse::Running{std::chrono::steady_clock::now(), ""};
            tick(); if (cx.frame("t" + st + "-e-run")) return true;
            auto started = std::get<ToolUse::Running>(et.status).started_at;
            et.status = ToolUse::Done{
                started, std::chrono::steady_clock::now(),
                "Edited src/gen_" + st + ".cpp (" + std::to_string(kHunks)
                    + " edits):\n```diff\n" + diff_body + "\n```"};
            et.expanded = false;
            // NO render here — the Done flip rides the same frame as the
            // continuation prose below.
        }
    }
    {
        Message cont; cont.role = Role::Assistant;
        m.d.current.messages.push_back(std::move(cont));
    }
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    if (stream_paras(cx, m.d.current.messages.back(), st, lead + 3, 3,
                     ("t" + st + "-e-post").c_str(), /*wrapping=*/true, /*burst=*/2))
        return true;

    return settle_freeze_trim(cx, st);
}

static int run_shape(int W, int H) {
    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::fprintf(err, "openpty failed\n"); return 2;
    }
    struct winsize ws{};
    ws.ws_col = (unsigned short)W; ws.ws_row = (unsigned short)H;
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    std::fprintf(err, "=== shape %dx%d ===\n", W, H);

    maya::RunConfig cfg;
    cfg.mode = maya::Mode::Inline;
    auto rt_r = maya::detail::Runtime::create(cfg);
    if (!rt_r) { std::fprintf(err, "Runtime::create failed\n"); return 2; }
    auto rt = std::move(*rt_r);

    TermEmu emu(W, H);
    emu.feed(read_all(master));

    Oracle orc;

    Model m;
    m.d.current.id = agentty::ThreadId{"oracle"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    Ctx cx{&rt, &emu, &orc, master, &m};

    if (cx.frame("welcome")) goto done;

    // Rotate prose / bash-tool / write+edit turns; enough to trigger the
    // trim twice. The write+edit turn covers the mutating-tool card's
    // tail-window→show_all settle expansion and the EditDiff→GitDiff
    // render-path switch, both while committed rows sit above the panel.
    {
        const int kTurns = 6;
        for (int t = 0; t < kTurns; ++t) {
            bool failed = (t % 3 == 0) ? prose_turn(cx, t, H)
                        : (t % 3 == 1) ? tool_turn(cx, t, H)
                                       : write_edit_turn(cx, t, H);
            if (failed) goto done;
        }
    }

    std::fprintf(err, "  [info] frames oracle-checked: %d\n", orc.frames_checked);

done:
    close(master);
    return 0;
}

int main() {
    setlocale(LC_ALL, "C.UTF-8");   // wcwidth needs a UTF-8 locale
    err = fdopen(dup(STDERR_FILENO), "w");
    const int shapes[][2] = {{80, 30}, {60, 18}, {100, 50}, {46, 76}};
    // ORACLE_SHAPE="60x18" runs a single shape (debug iteration).
    const char* only = std::getenv("ORACLE_SHAPE");
    for (auto& s : shapes) {
        if (only) {
            const std::string want = std::to_string(s[0]) + "x" + std::to_string(s[1]);
            if (want != only) continue;
        }
        if (run_shape(s[0], s[1]) == 2) return 2;
    }
    if (g_failures == 0)
        std::fprintf(err, "PASS: append-only oracle + markers + chrome (all shapes)\n");
    else
        std::fprintf(err, "FAILED: %d corruption(s) detected\n", g_failures);
    return g_failures ? 1 : 0;
}
