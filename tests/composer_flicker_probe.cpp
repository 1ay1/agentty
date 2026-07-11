// composer_flicker_probe — reproduce "while rendering lists some things are
// hidden first and appear later; the composer flickers a lot".
//
// Drives the REAL agentty view + maya Runtime::render through a PTY while an
// assistant turn streams a LONG TIGHT LIST (the dominant reply shape), with a
// minimal in-process ANSI emulator tracking the viewport. Per frame:
//   • locate the composer top border row; count frames where it MOVED and
//     frames where its bytes were REWRITTEN in place (flicker),
//   • detect content rows that were NON-BLANK, went BLANK, then later became
//     non-blank again at the same visual position (hidden → appears later),
//   • count rows rewritten per frame (repaint churn).
//
// Run: ./build/composer_flicker_probe [total_paras] [bytes_per_frame]
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;

// ── ANSI emulator (ported from scrollback_oracle_test: wcwidth-faithful) ──
struct Emu {
    int cols, rows;
    int cx = 0, cy = 0;
    std::vector<std::string> screen;
    std::vector<std::string> scrollback;
    bool decawm = true;
    std::string carry;

    Emu(int c, int r) : cols(c), rows(r) {
        screen.assign((size_t)rows, std::string((size_t)cols, ' '));
    }
    std::string& line(int y) { return screen[(size_t)y]; }

    void put_cp(char32_t cp, int w) {
        if (w <= 0) return;
        if (cx + w > cols) {
            if (decawm) { cx = 0; newline(); }
            else        { cx = cols - w; if (cx < 0) cx = 0; }
        }
        if (cy < 0) cy = 0;
        if (cy >= rows) cy = rows - 1;
        std::string& l = line(cy);
        if ((int)l.size() < cols) l.resize((size_t)cols, ' ');
        const char vis = (cp < 128) ? (char)cp : '#';
        l[(size_t)cx] = vis;
        for (int k = 1; k < w && cx + k < cols; ++k)
            l[(size_t)(cx + k)] = (cp < 128) ? ' ' : '#';
        cx += w;
    }
    void newline() {
        if (cy < rows - 1) { ++cy; return; }
        scrollback.push_back(screen.front());
        screen.erase(screen.begin());
        screen.push_back(std::string((size_t)cols, ' '));
    }
    static constexpr size_t kInc = (size_t)-1;
    size_t esc(const std::string& b, size_t i) {
        const size_t n = b.size();
        if (i + 1 >= n) return kInc;
        if (b[i + 1] == ']') {
            size_t j = i + 2;
            while (j < n && b[j] != '\x07'
                   && !(b[j] == '\x1b' && j + 1 < n && b[j + 1] == '\\')) ++j;
            if (j < n && b[j] == '\x07') return j + 1;
            if (j + 1 < n) return j + 2;
            return kInc;
        }
        if (b[i + 1] != '[') return i + 2;
        size_t j = i + 2;
        bool priv = false;
        if (j < n && b[j] == '?') { priv = true; ++j; }
        int p[4] = {0,0,0,0}; bool hv[4] = {false,false,false,false}; int pi = 0;
        while (j < n && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';')) {
            if (b[j] == ';') { if (pi < 3) ++pi; }
            else { p[pi] = p[pi]*10 + (b[j]-'0'); hv[pi] = true; }
            ++j;
        }
        if (j >= n) return kInc;
        char f = b[j];
        const int a = hv[0] ? p[0] : 0;
        switch (f) {
            case 'A': cy -= a ? a : 1; if (cy < 0) cy = 0; break;
            case 'B': for (int k = 0; k < (a ? a : 1); ++k) newline(); break;
            case 'C': cx += a ? a : 1; if (cx > cols) cx = cols; break;
            case 'D': cx -= a ? a : 1; if (cx < 0) cx = 0; break;
            case 'G': cx = a ? a - 1 : 0; if (cx < 0) cx = 0; break;
            case 'H': case 'f': {
                int r = hv[0] && p[0] ? p[0] : 1, c = hv[1] && p[1] ? p[1] : 1;
                cy = std::clamp(r - 1, 0, rows - 1);
                cx = std::clamp(c - 1, 0, cols);
                break;
            }
            case 'K': {
                auto& l = line(cy);
                if ((int)l.size() < cols) l.resize((size_t)cols, ' ');
                if (a == 2) l.assign((size_t)cols, ' ');
                else if (a == 1) for (int x = 0; x <= cx && x < cols; ++x) l[(size_t)x] = ' ';
                else for (int x = cx; x < cols; ++x) l[(size_t)x] = ' ';
                break;
            }
            case 'J':
                if (a == 2 || a == 3) {
                    if (a == 3) scrollback.clear();
                    for (auto& l : screen) l.assign((size_t)cols, ' ');
                } else {
                    auto& l = line(cy);
                    if ((int)l.size() < cols) l.resize((size_t)cols, ' ');
                    for (int x = cx; x < cols; ++x) l[(size_t)x] = ' ';
                    for (int y = cy + 1; y < rows; ++y)
                        screen[(size_t)y].assign((size_t)cols, ' ');
                }
                break;
            case 'h': if (priv && a == 7) decawm = true; break;
            case 'l': if (priv && a == 7) decawm = false; break;
            default: break;
        }
        return j + 1;
    }
    void feed(const std::string& chunk) {
        std::string b = std::move(carry);
        carry.clear();
        b += chunk;
        size_t i = 0, n = b.size();
        while (i < n) {
            unsigned char ch = (unsigned char)b[i];
            if (ch == '\r') { cx = 0; ++i; continue; }
            if (ch == '\n') { newline(); ++i; continue; }
            if (ch == 0x1b) {
                size_t adv = esc(b, i);
                if (adv == kInc) { carry = b.substr(i); return; }
                i = adv; continue;
            }
            if (ch < 0x20) { ++i; continue; }
            char32_t cp = 0; int len = 1;
            if      (ch < 0x80)       { cp = ch; len = 1; }
            else if ((ch >> 5) == 0x6)  len = 2;
            else if ((ch >> 4) == 0xe)  len = 3;
            else if ((ch >> 3) == 0x1e) len = 4;
            else { ++i; continue; }
            if (len > 1) {
                if (i + (size_t)len > n) { carry = b.substr(i); return; }
                cp = (char32_t)(ch & (0xff >> (len + 1)));
                for (int k = 1; k < len; ++k)
                    cp = (cp << 6) | (char32_t)((unsigned char)b[i+(size_t)k] & 0x3f);
            }
            i += (size_t)len;
            int w = (cp < 128) ? 1 : wcwidth((wchar_t)cp);
            if (w < 0) w = 1;
            put_cp(cp, w);
        }
    }
};

static void tick(int t = 16) { maya::testing::advance_anim_clock_ms(t); }

static std::string read_all(int fd) {
    std::string s; char buf[16384];
    for (;;) { ssize_t n = ::read(fd, buf, sizeof buf); if (n <= 0) break; s.append(buf, (size_t)n); }
    return s;
}

static bool row_blank(const std::string& r) {
    return r.find_first_not_of(' ') == std::string::npos;
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "C.UTF-8");
    const int paras = argc > 1 ? std::atoi(argv[1]) : 120;
    const int bytes_per_frame = argc > 2 ? std::atoi(argv[2]) : 96;
    const int W = 100, H = 40;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed\n"); return 2;
    }
    struct winsize ws{}; ws.ws_col = W; ws.ws_row = H;
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, STDIN_FILENO); dup2(slave, STDOUT_FILENO); close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    maya::RunConfig cfg; cfg.mode = maya::Mode::Inline;
    auto rt_r = maya::detail::Runtime::create(cfg);
    if (!rt_r) { std::fprintf(stderr, "rt create failed\n"); return 2; }
    auto rt = std::move(*rt_r);

    Emu emu(W, H);

    Model m;
    m.d.current.id = agentty::ThreadId{"flick"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    std::vector<size_t> read_sizes;
    int residue_frames = 0;
    auto render = [&] {
        maya::Element root = agentty::ui::view(m);
        (void)rt.render(root);
        std::string bytes = read_all(master);
        // Drain deferred bytes: the PTY buffer is small, so a frame can be
        // split (writer residue). A real terminal sees the remainder a few
        // ms later — inside the same DEC-2026 sync window on supporting
        // terminals. Diffing a torn frame here would count phantom
        // "flicker", so drain until the frame is complete.
        bool had_residue = rt.has_pending_writes();
        for (int k = 0; k < 64 && rt.has_pending_writes(); ++k) {
            (void)rt.render(root);   // residue-drain path (no recompose)
            bytes += read_all(master);
        }
        if (had_residue) ++residue_frames;
        read_sizes.push_back(bytes.size());
        emu.feed(bytes);
    };
    render();   // welcome

    std::vector<int> content_rows;
    int frames = 0;
    std::vector<int> view_rows;
    maya::StylePool vpool;
    std::vector<std::string> prev_vrows;
    // OPT-IN: measure_view renders the view tree into a PRIVATE canvas with
    // a PRIVATE StylePool — but maya's component cell-cache is thread_local
    // and keyed on hash_id, so cells captured under the private pool's style
    // ids can be blitted into the runtime's canvas (wrong styles → shadow
    // poison → VERIFY-DEMOTE). Only enable when hunting view-tree height
    // oscillation, and ignore the runtime metrics on such runs.
    static const bool measure_enabled =
        std::getenv("FLICK_MEASURE_VIEW") != nullptr;
    auto measure_view = [&]() -> int {
        if (!measure_enabled) return 0;
        maya::RenderContext ctx{W, H, maya::render_generation(),
                                /*auto_height=*/true};
        maya::RenderContextGuard g(ctx);
        maya::Element el = agentty::ui::view(m);
        maya::Canvas cv(W, 4000, &vpool);
        maya::render_tree(el, cv, vpool, maya::theme::dark, true);
        const int h = maya::content_height(cv);
        // capture rows for shrink diagnosis
        std::vector<std::string> rows;
        rows.reserve((size_t)h);
        for (int y = 0; y < h; ++y) {
            std::string out;
            for (int x = 0; x < W; ++x) {
                auto cell = cv.get(x, y);
                if (cell.width == 2) continue;
                char32_t cp = cell.character;
                out += (cp < 0x80) ? (char)cp : '#';
            }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            rows.push_back(std::move(out));
        }
        if (!view_rows.empty() && h < view_rows.back()) {
            static int vdumps = 0;
            if (vdumps < 3) {
                ++vdumps;
                std::fprintf(stderr,
                    "--- view SHRINK %d -> %d (frame %d) tail rows ---\n",
                    view_rows.back(), h, frames);
                const int lo = std::max(0, h - 14);
                std::fprintf(stderr, "  NOW:\n");
                for (int y = lo; y < h; ++y)
                    std::fprintf(stderr, "   %3d|%s\n", y, rows[(size_t)y].c_str());
                std::fprintf(stderr, "  PREV:\n");
                const int plo = std::max(0, (int)prev_vrows.size() - 16);
                for (int y = plo; y < (int)prev_vrows.size(); ++y)
                    std::fprintf(stderr, "   %3d|%s\n", y, prev_vrows[(size_t)y].c_str());
            }
        }
        prev_vrows = std::move(rows);
        return h;
    };

    { // submit
        Message u; u.role = Role::User;
        u.text = "stream a long list";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        render();
    }

    // The long tight list body.
    std::string doc = "Here is the breakdown of everything that matters:\n\n";
    for (int i = 1; i <= paras; ++i)
        doc += "- item " + std::to_string(i)
             + " with some **bold** text and `code` tokens that make the "
               "row wrap once at a hundred columns of width\n";

    Message a; a.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(a));

    // ── Stream + measure ──
    size_t fed = 0;
    std::vector<std::string> prev = emu.screen;
    int prev_comp_row = -1;
    std::string prev_comp_bytes;

    int comp_moves = 0, comp_rewrites = 0, comp_bounces = 0;
    int hidden_reappear = 0;
    long rows_rewritten_total = 0;
    std::vector<int> comp_traj;
    std::vector<int> first_seen;
    struct Ev { int f; std::string what; };
    std::vector<Ev> log;

    // Track per-row nonblank→blank transitions (viewport-position keyed).
    std::vector<int> blank_since(H, -1);       // frame the row went blank
    std::vector<std::string> before_blank(H);  // its content before blanking

    auto find_composer = [&]() -> int {
        // composer top border: a row of '#' (was ╭───╮) spanning most of the
        // width, searched bottom-up above the status bar.
        for (int y = H - 1; y >= 0; --y) {
            const std::string& r = emu.screen[(size_t)y];
            int hashes = 0;
            for (char c : r) if (c == '#') ++hashes;
            if (hashes > W / 2) return y;   // border-ish row
        }
        return -1;
    };

    while (fed < doc.size()) {
        size_t n = std::min((size_t)bytes_per_frame, doc.size() - fed);
        m.d.current.messages.back().streaming_text.append(doc, fed, n);
        fed += n;
        tick();
        render();
        ++frames;
        content_rows.push_back(rt.inline_content_rows());
        view_rows.push_back(measure_view());

        // rows rewritten this frame
        int rw = 0;
        std::string rw_rows;
        for (int y = 0; y < H; ++y)
            if (emu.screen[(size_t)y] != prev[(size_t)y]) {
                ++rw;
                if (frames >= 60 && frames <= 70) {
                    rw_rows += std::to_string(y);
                    rw_rows += ' ';
                }
            }
        rows_rewritten_total += rw;
        if (frames >= 60 && frames <= 70)
            std::fprintf(stderr, "  [f%d] rewrote %d rows: %s (content_rows=%d comp=%d)\n",
                         frames, rw, rw_rows.c_str(),
                         rt.inline_content_rows(), prev_comp_row);

        // composer tracking
        int cr = find_composer();
        if (cr >= 0) {
            comp_traj.push_back(cr);
            const std::string& cb = emu.screen[(size_t)cr];
            if (prev_comp_row >= 0) {
                if (cr != prev_comp_row) {
                    ++comp_moves;
                    if (cr < prev_comp_row) {
                        ++comp_bounces;   // composer jumped UP — flicker
                        if (log.size() < 30)
                            log.push_back({frames,
                                "composer UP " + std::to_string(prev_comp_row)
                                + " -> " + std::to_string(cr)});
                        static int dumps = 0;
                        if (dumps < 4 && frames > 30) {
                            ++dumps;
                            std::fprintf(stderr,
                                "--- bounce f%d (comp %d -> %d) read=%zu prev_read=%zu ---\n",
                                frames, prev_comp_row, cr,
                                read_sizes.back(),
                                read_sizes.size() > 1
                                    ? read_sizes[read_sizes.size() - 2] : 0);
                        }
                    }
                } else if (cb != prev_comp_bytes) {
                    ++comp_rewrites;
                }
            }
            prev_comp_row = cr; prev_comp_bytes = cb;
        }

        // hidden→reappear detection (viewport position; ignore rows that
        // simply scrolled — approximate by only flagging rows ABOVE the
        // composer that blank out while the frame is otherwise stable).
        for (int y = 0; y < (cr >= 0 ? cr : H); ++y) {
            const bool was = !row_blank(prev[(size_t)y]);
            const bool now = !row_blank(emu.screen[(size_t)y]);
            if (was && !now) {
                blank_since[(size_t)y] = frames;
                before_blank[(size_t)y] = prev[(size_t)y];
            } else if (!was && now && blank_since[(size_t)y] >= 0) {
                const int gap = frames - blank_since[(size_t)y];
                if (gap >= 3) {   // hidden for >= 3 frames then re-appeared
                    ++hidden_reappear;
                    if (log.size() < 20)
                        log.push_back({frames,
                            "row " + std::to_string(y) + " hidden " +
                            std::to_string(gap) + "f then reappeared:\n"
                            "      before-blank: [" +
                            before_blank[(size_t)y].substr(0, 70) + "]\n"
                            "      now:          [" +
                            emu.screen[(size_t)y].substr(0, 70) + "]\n"
                            "      row above now:[" +
                            (y > 0 ? emu.screen[(size_t)y-1].substr(0, 70)
                                   : std::string{}) + "]\n"
                            "      row below now:[" +
                            (y + 1 < H ? emu.screen[(size_t)y+1].substr(0, 70)
                                       : std::string{}) + "]"});
                }
                blank_since[(size_t)y] = -1;
            }
        }
        prev = emu.screen;

        // First-visible frame per list item (viewport + scrollback). If item
        // N first shows AFTER item N+1, earlier content was HIDDEN while
        // later content displayed — the reported "hidden first, appears
        // later" artefact.
        {
            auto visible = [&](const std::string& needle) {
                for (auto& r : emu.screen)
                    if (r.find(needle) != std::string::npos) return true;
                for (auto& r : emu.scrollback)
                    if (r.find(needle) != std::string::npos) return true;
                return false;
            };
            if ((int)first_seen.size() < paras + 1)
                first_seen.resize((size_t)paras + 1, -1);
            // only probe items near the fed edge (cheap)
            int hi = std::min(paras, (int)(fed / 80) + 4);
            for (int it = std::max(1, hi - 12); it <= hi; ++it) {
                if (first_seen[(size_t)it] >= 0) continue;
                if (visible("item " + std::to_string(it) + " "))
                    first_seen[(size_t)it] = frames;
            }
        }
    }

    // settle
    for (int k = 0; k < 300; ++k) {
        tick(); render();
        if (agentty::app::detail::live_tail_reveal_settled(m)) break;
    }

    std::fprintf(stderr,
        "frames=%d residue_frames=%d  composer: moves=%d (UP-bounces=%d) in-place-rewrites=%d\n"
        "hidden->reappear rows=%d   rows rewritten/frame avg=%.1f\n",
        frames, residue_frames, comp_moves, comp_bounces, comp_rewrites, hidden_reappear,
        frames ? (double)rows_rewritten_total / frames : 0.0);
    // Composer row trajectory (compact run-length print).
    std::fprintf(stderr, "composer row trajectory: ");
    for (size_t i = 0; i < comp_traj.size();) {
        size_t j = i;
        while (j < comp_traj.size() && comp_traj[j] == comp_traj[i]) ++j;
        std::fprintf(stderr, "%d×%zu ", comp_traj[i], j - i);
        i = j;
    }
    std::fprintf(stderr, "\n");
    // Content-rows trajectory + shrink events (widget-height oscillation).
    int height_shrinks = 0, worst_hshrink = 0;
    for (size_t i = 1; i < content_rows.size(); ++i) {
        int d = content_rows[i] - content_rows[i - 1];
        if (d < 0) {
            ++height_shrinks;
            worst_hshrink = std::max(worst_hshrink, -d);
            if (height_shrinks <= 20)
                std::fprintf(stderr, "  f%zu content rows %d -> %d\n",
                             i + 1, content_rows[i - 1], content_rows[i]);
        }
    }
    std::fprintf(stderr, "content-height shrink events=%d worst=%d\n",
                 height_shrinks, worst_hshrink);
    int vshrinks = 0, worst_vshrink = 0;
    for (size_t i = 1; i < view_rows.size(); ++i) {
        int d = view_rows[i] - view_rows[i - 1];
        if (d < 0) { ++vshrinks; worst_vshrink = std::max(worst_vshrink, -d); }
    }
    std::fprintf(stderr, "view-height shrink events=%d worst=%d\n",
                 vshrinks, worst_vshrink);
    // Wire bytes per frame — the real flicker driver: large frames back up
    // the tty (WouldBlock → residue → torn presentation past the terminal's
    // DEC-2026 sync timeout).
    {
        std::vector<size_t> s = read_sizes;
        std::sort(s.begin(), s.end());
        auto pct = [&](double p) {
            return s.empty() ? (size_t)0
                : s[std::min(s.size() - 1, (size_t)(p * (s.size() - 1)))];
        };
        size_t sum = 0; for (auto v : s) sum += v;
        std::fprintf(stderr,
            "wire bytes/frame: mean=%zu p50=%zu p95=%zu max=%zu total=%zu\n",
            s.empty() ? 0 : sum / s.size(), pct(0.5), pct(0.95),
            s.empty() ? 0 : s.back(), sum);
    }
    // Out-of-order first-visibility.
    {
        int ooo = 0;
        for (size_t it = 1; it + 1 < first_seen.size(); ++it) {
            if (first_seen[it] < 0 || first_seen[it + 1] < 0) continue;
            if (first_seen[it] > first_seen[it + 1]) {
                ++ooo;
                if (ooo <= 10)
                    std::fprintf(stderr,
                        "  OOO: item %zu first seen f%d AFTER item %zu (f%d)\n",
                        it, first_seen[it], it + 1, first_seen[it + 1]);
            }
        }
        std::fprintf(stderr, "out-of-order item appearances=%d\n", ooo);
    }
    for (auto& e : log)
        std::fprintf(stderr, "  f%d %s\n", e.f, e.what.c_str());
    close(master);
    return 0;
}
