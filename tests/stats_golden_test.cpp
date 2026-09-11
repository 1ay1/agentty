// stats_golden_test — byte-identity guard for every stats tab, at every
// width and height that changes its layout.
//
// WHY THIS EXISTS
// ===============
// The stats viewer is about to be ported from its own StatSheet -- which
// re-implements chrome, viewport, width negotiation and column layout that
// maya::Panel already owns -- onto the panel family's Item/Control
// vocabulary. That is a rewrite of the render path, and a rewrite is
// exactly the change a reading of the diff cannot verify.
//
// Three separate attempts at improving this subsystem shipped regressions
// that the existing tests were all green for: a bar that swallowed its own
// value column, a track frozen at 14 cells, a scroll offset the frame gate
// could not see. Each time the fix looked right in the diff. What was
// missing was a record of what the pixels USED to be.
//
// So this pins the pixels. Not "does it look reasonable" -- that asserts
// nothing -- but "these exact cells, and if any of them move you must say
// why".
//
// HOW TO USE IT WHEN IT FAILS
// ===========================
// A failure is not automatically a bug. It says the rendering changed.
//   1. Run tests/stats_visual at the failing width to SEE the change.
//   2. If the change is wrong, fix it. That is the case this exists for.
//   3. If the change is right, set kGoldenHash to 0, run once to print the
//      new hash, and paste it back -- in the same commit as the change, so
//      the diff shows a pixel change was intended and reviewed.
//
// The per-tab feature assertions below are the part that survives a
// deliberate re-pin: they say what the output must CONTAIN regardless of
// its exact bytes, so a re-pinned hash cannot quietly drop a whole section.

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/domain/stats/tabs.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;
using namespace agentty;

namespace {

std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

// A FIXED transcript. Every number the panel prints is derived from this,
// so the golden bytes are a function of the renderer alone.
//
// Deliberately varied: several models, a routed and an unrouted turn, a
// failed tool, cache hits and misses, reasoning tokens, and a long tool
// name (git_status) -- because a table's label column is as wide as its
// widest entry, and a fixture of four-character names cannot exercise the
// case where a long label crowds the track.
Model golden_model() {
    Model m;
    m.d.current.id = ThreadId{"golden-stats"};

    auto turn = [&](const char* model, std::uint32_t in, std::uint32_t out,
                    std::uint32_t cache_r, std::uint32_t cache_w,
                    std::uint32_t ttft, std::uint32_t stream,
                    std::uint32_t think) {
        Message u;
        u.role = Role::User;
        u.text = "a question";
        m.d.current.messages.push_back(std::move(u));

        Message a;
        a.role         = Role::Assistant;
        a.served_model = ModelId{model};
        a.text         = "an answer with some prose in it";
        Message::Telemetry t;
        t.input_tokens     = in;
        t.output_tokens    = out;
        t.cache_read       = cache_r;
        t.cache_creation   = cache_w;
        t.ttft_ms          = ttft;
        t.stream_ms        = stream;
        t.reasoning_tokens = think;
        a.telemetry = t;
        return &m.d.current.messages.emplace_back(std::move(a));
    };

    turn("claude-opus-4-5",   8200, 1400,     0, 8200, 1200, 9400, 900);
    turn("claude-sonnet-4-5",  900,  600, 15800,    0,  240, 2100,   0);
    turn("claude-sonnet-4-5", 1100,  480, 16200,    0,  180, 1700,   0);
    turn("claude-haiku-4-5",   500,  260, 15900,    0,  120,  700,   0);
    return m;
}

// A LONG session, the shape a real one reaches.
//
// The fixture above is four turns with tidy numbers, and every value it
// produces is short: "4 turns", "16s", "2.1k". Real threads are not like
// that — a screenshot of a working session showed 2223 turns and 3h33m,
// where the counts are four digits, the durations carry two units, and
// every string in the panel is wider than anything this file had ever
// rendered.
//
// That matters because width bugs are triggered by CONTENT width, not by
// terminal width, and a fixture whose values are all short cannot reach
// them. A tidy fixture does not test a layout; it tests the layout's
// easiest case and reports that as a pass.
//
// Deliberately awkward: four-digit counts, hour-scale durations, a model
// name long enough to compete for the label lane, and reasoning tokens so
// the "of which reasoning" sub-row appears.
Model heavy_model() {
    Model m;
    m.d.current.id = ThreadId{"heavy-stats"};

    auto turn = [&](const char* model, std::uint32_t in, std::uint32_t out,
                    std::uint32_t cache_r, std::uint32_t cache_w,
                    std::uint32_t ttft, std::uint32_t stream,
                    std::uint32_t think) {
        Message u;
        u.role = Role::User;
        u.text = "a question";
        m.d.current.messages.push_back(std::move(u));

        Message a;
        a.role         = Role::Assistant;
        a.served_model = ModelId{model};
        a.text         = "an answer with some prose in it";
        Message::Telemetry t;
        t.input_tokens     = in;
        t.output_tokens    = out;
        t.cache_read       = cache_r;
        t.cache_creation   = cache_w;
        t.ttft_ms          = ttft;
        t.stream_ms        = stream;
        t.reasoning_tokens = think;
        a.telemetry = t;
        return &m.d.current.messages.emplace_back(std::move(a));
    };

    // Enough turns for four-digit counts and hour-scale totals.
    for (int i = 0; i < 400; ++i) {
        turn("claude-opus-4-5",    9100, 2300, 48000, 9100, 2400, 31000, 4100);
        turn("claude-sonnet-4-5",  7400, 1900, 52000,    0, 1100, 18000,    0);
        turn("claude-haiku-4-5",   3300,  850, 41000,    0,  420,  6200,    0);
    }
    return m;
}

// Flatten the panel to text at a given width and height.
// Text the stats panel had CLIPPED AWAY, across every tab and width.
//
// Filled by render_at while the frames are painted, asserted once at the
// end. This is the one fact about a frame that cannot be recovered from
// the frame: the golden hash, the overflow scan and every feature check
// read what LANDED on the canvas, and a canvas cannot show you what was
// never drawn. A row whose value was clipped leaves a tidy-looking frame
// that is missing a number — which is precisely how a week of width bugs
// stayed invisible to this file while being obvious in a screenshot.
std::vector<std::string> g_clipped;

std::string render_at(const Model& m, int w, int rows) {
    // Panel clamps its own min_width to the terminal and reads COLUMNS when
    // there is no tty; LINES drives the viewport the same way. Both must be
    // set or the golden bytes are a function of the machine, not the code.
    const std::string cols = std::to_string(w);
    const std::string lns  = std::to_string(rows);
    setenv("COLUMNS", cols.c_str(), 1);
    setenv("LINES",   lns.c_str(),  1);

    maya::StylePool pool;
    maya::Canvas canvas(w, rows, &pool);
    canvas.on_clip_overflow([w, rows](const maya::Canvas::ClipOverflow& o) {
        g_clipped.push_back(std::to_string(w) + "x" + std::to_string(rows)
                            + " dropped '" + std::string(o.text)
                            + "' (wanted " + std::to_string(o.wanted)
                            + " cols, edge at " + std::to_string(o.edge) + ")");
    });
    maya::render_tree(ui::stats_panel(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);

    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < w; ++x) {
            const char32_t c = canvas.get(x, y).character;
            // UTF-8 encode: the figures are braille and box-drawing, and
            // folding them to '?' would make the hash blind to exactly the
            // glyphs this panel is made of.
            const char32_t ch = c ? c : U' ';
            if (ch < 0x80) out += static_cast<char>(ch);
            else if (ch < 0x800) {
                out += static_cast<char>(0xC0 | (ch >> 6));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                out += static_cast<char>(0xE0 | (ch >> 12));
                out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (ch >> 18));
                out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        out += '\n';
    }
    return out;
}

// Every tab, at the widths and heights that change the layout.
//
// The widths bracket the decisions the sheet makes: 40 is below any split,
// 68 is the reported phone-pane width, 76/90 straddle the split threshold,
// 120/200 are where a wide surface should be used rather than left blank.
// The heights matter because column count is a response to VERTICAL
// pressure -- a tab that fits does not split, whatever its width.
//
// Model is move-only (it owns a scrollback ledger and a view cache), which
// is the right call for a type this size -- so each case builds its own
// rather than copying a base. The fixture is deterministic, so rebuilding
// is equivalent to copying and costs nothing that matters here.
std::string render_all() {
    const int widths[]  = {40, 60, 68, 76, 90, 120, 200};
    const int heights[] = {24, 40, 60};

    std::string all;
    for (const auto& desc : stats::kTabs) {
        for (int w : widths)
            for (int h : heights) {
                auto [opened, _] =
                    app::update(golden_model(), Msg{OpenStats{}});
                if (auto* o = opened.ui.panel.get<pn::Stats>())
                    o->tab = desc.id;
                all += "== ";
                all += stats::tab_title(desc.id);
                all += " " + std::to_string(w) + "x" + std::to_string(h) + " ==\n";
                all += render_at(opened, w, h);
            }
    }
    return all;
}

}  // namespace

TEST_CASE("stats_golden: every tab renders byte-identically") {
    install_stub_deps();
    const std::string out = render_all();

    // STATS_DUMP=<tab> prints the rendered frames for a tab and stops.
    //
    // The hash says THAT the rendering changed; this says WHAT it changed
    // to, which is the question you actually have when it fails. It reuses
    // this harness rather than living in stats_visual because the fixture
    // here is the one the assertions are written against — a second fixture
    // is a second thing to keep true.
    //
    //   STATS_DUMP=Tools ctest -R stats_golden --output-on-failure
    //   STATS_DUMP=all   …            every tab
    if (const char* want = std::getenv("STATS_DUMP")) {
        const std::string sel = want;
        std::size_t pos = 0;
        bool on = false;
        while (pos < out.size()) {
            const std::size_t nl = out.find('\n', pos);
            if (nl == std::string::npos) break;
            const std::string line = out.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.rfind("== ", 0) == 0)
                on = (sel == "all") || line.find(sel) != std::string::npos;
            if (on) std::fprintf(stderr, "%s\n", line.c_str());
        }
    }

    // ── What the panel threw away ────────────────────────────────
    //
    // Every other check in this file reads the painted frames, so none of
    // them can see a cell that was never painted. A clipped value leaves a
    // frame that looks right and is missing a number, which is how a week
    // of width bugs survived a green suite here.
    //
    // ASSERTED, not merely reported, because the panel is clip-clean and
    // that is worth keeping. The first run of this check found nine drops
    // the golden hash had been happily pinning — three subtitles cut dead
    // at 40 columns with no ellipsis, so the sentence just stopped and the
    // reader had no reason to think a word was missing.
    //
    // A deliberate truncation does NOT trip this: an ellipsised label has
    // already shortened itself to fit, so nothing reaches the clip edge.
    // That is the distinction this assertion is built on — it fires on
    // information lost WITHOUT the reader being told, and stays quiet on
    // information the panel chose to abbreviate.
    if (!g_clipped.empty()) {
        std::fprintf(stderr, "\nstats: %zu clipped write(s) across the sweep\n",
                     g_clipped.size());
        std::size_t shown = 0;
        for (const auto& c : g_clipped) {
            if (shown++ >= 12) {
                std::fprintf(stderr, "  … and %zu more\n",
                             g_clipped.size() - 12);
                break;
            }
            std::fprintf(stderr, "  %s\n", c.c_str());
        }
    }
    check(g_clipped.empty(),
          "no stats text is cut off without an ellipsis");

    // The same question asked of a LONG session.
    //
    // Width bugs are triggered by content width, not terminal width, so a
    // fixture whose every value is short ("4 turns", "16s") cannot reach
    // them — it exercises the layout's easiest case and reports that as a
    // pass. A real thread carries four-digit counts and hour-scale
    // durations, which is where the strings actually compete for the row.
    //
    // Not hashed, deliberately. Pinning the bytes of a 1200-turn fixture
    // would make every unrelated formatting change a re-pin chore for no
    // extra safety; what is worth asserting is the property, and the
    // property is that nothing is lost without the reader being told.
    g_clipped.clear();
    // HEAVY_DUMP=<width> prints the heavy fixture at one width, which is
    // the shape a real session has: four-digit counts, hour-scale
    // durations, values wide enough to compete for the row.
    if (const char* hw = std::getenv("HEAVY_DUMP")) {
        const int w = std::atoi(hw);
        for (const auto& desc : stats::kTabs) {
            auto [opened, _] = app::update(heavy_model(), Msg{OpenStats{}});
            if (auto* o = opened.ui.panel.get<pn::Stats>()) o->tab = desc.id;
            std::fprintf(stderr, "== %s %dx40 ==\n",
                         std::string{stats::tab_title(desc.id)}.c_str(), w);
            std::fprintf(stderr, "%s", render_at(opened, w, 40).c_str());
        }
    }
    for (const auto& desc : stats::kTabs) {
        for (int w : {40, 60, 76, 90, 120, 200}) {
            auto [opened, _] = app::update(heavy_model(), Msg{OpenStats{}});
            if (auto* o = opened.ui.panel.get<pn::Stats>())
                o->tab = desc.id;
            (void)render_at(opened, w, 40);
        }
    }
    if (!g_clipped.empty()) {
        std::fprintf(stderr, "\nstats (heavy session): %zu clipped write(s)\n",
                     g_clipped.size());
        std::size_t shown = 0;
        for (const auto& c : g_clipped) {
            if (shown++ >= 12) {
                std::fprintf(stderr, "  … and %zu more\n",
                             g_clipped.size() - 12);
                break;
            }
            std::fprintf(stderr, "  %s\n", c.c_str());
        }
    }
    check(g_clipped.empty(),
          "a long session loses no text either");

    // ── Feature assertions ──────────────────────────────────
    //
    // What the output must CONTAIN, regardless of its exact bytes. These
    // are what survives a deliberate re-pin: a new hash cannot quietly
    // drop a section, because these still have to hold.
    check(out.find("Session") != std::string::npos, "the Session tab is drawn");
    check(out.find("Tokens")  != std::string::npos, "the Tokens tab is drawn");
    check(out.find("Cache")   != std::string::npos, "the Cache tab is drawn");
    check(out.find("claude-sonnet-4-5") != std::string::npos,
          "model names reach the Models tab");
    check(out.find("\xe2\x96\x88") != std::string::npos,
          "bars are drawn (a stats row without its chart is the frozen-track bug)");
    check(out.find("\xe2\x94\x82") != std::string::npos,
          "the panel frame is drawn");

    // No row may exceed the width it was rendered at -- the frame-overdraw
    // class, pinned here too because this harness sees every tab at every
    // size. Each block is preceded by a "== Tab WxH ==" marker, so the
    // expected width is in the stream itself.
    {
        int over = 0;
        int want = 0;
        std::size_t pos = 0;
        while (pos < out.size()) {
            const std::size_t nl = out.find('\n', pos);
            if (nl == std::string::npos) break;
            const std::string line = out.substr(pos, nl - pos);
            pos = nl + 1;

            if (line.rfind("== ", 0) == 0) {
                // "== Tab 76x40 ==" -- take the width between space and 'x'.
                const std::size_t x = line.rfind('x');
                const std::size_t sp = line.rfind(' ', x);
                if (x != std::string::npos && sp != std::string::npos)
                    want = std::atoi(line.c_str() + sp + 1);
                continue;
            }
            if (want <= 0) continue;
            // Display width, not bytes: the figures are multi-byte.
            int cells = 0;
            for (std::size_t i = 0; i < line.size(); ) {
                const unsigned char c = static_cast<unsigned char>(line[i]);
                i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                ++cells;
            }
            if (cells > want) ++over;
        }
        check(over == 0, "no row overruns the width it was rendered at");
    }

    // ── Byte identity ────────────────────────────────────────────────
    //
    // Set to 0 to bootstrap: the run prints the hash to stderr, paste it
    // back IN THE SAME COMMIT as the change that moved it.
    const std::uint64_t kGoldenHash = 0xb78fa52f04296a5cull;
    const std::uint64_t got = fnv1a(out);

    if (kGoldenHash == 0) {
        std::fprintf(stderr,
            "GOLDEN stats hash = 0x%016llxull  (len=%zu)\n",
            static_cast<unsigned long long>(got), out.size());
    } else {
        check(got == kGoldenHash,
              "stats rendering is byte-identical to golden");
        if (got != kGoldenHash)
            std::fprintf(stderr,
                "stats golden MISMATCH: got 0x%016llxull want 0x%016llxull\n"
                "  Run ./build/stats_visual <width> <tab> to see the change.\n"
                "  If it is intended, re-pin the hash in the same commit.\n",
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(kGoldenHash));
    }
}
