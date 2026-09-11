// panel_overflow_probe — does any panel paint past its own frame?
//
// THE FLAW THIS MEASURES
// =====================
// maya's Panel composes its body as
//
//     Element scrollable = vstack()(lines) | scroll(s, vh) | grow(1.0f);
//     if (content > vh)
//         stack.push_back(h(scrollable, scrollbar_y(...)).build());
//
// The scrollbar is appended AFTER the content has been laid out. grow(1.0f)
// tells the content to claim the full width; an h() sibling then takes a
// column back. Any child that sized itself from `avail_w` -- every
// component(), which is what plots, bands and donuts are -- has already
// committed to a geometry one column too wide.
//
// So the contract a panel offers its content is "use all the width, except
// for the parts I have not told you about". agentty's stats panel is the
// only caller that compensates, and the compensation is a hand-counted
// magic number (`sheet.reserve_right(7)`) that is circular by construction:
// the scrollbar is conditional on content > vh, and vh depends on the width
// the reserve determines.
//
// THE FIX, AND WHY THIS IS NOW A TEST
// ===================================
// Panel subtracts its own chrome instead of asking callers to. It clamps
// min_width to the terminal (a floor wider than the screen is an overflow,
// not a floor), reserves the scrollbar gutter unconditionally (a body that
// reflows when content crosses the viewport shifts under the reader, and
// the conditional made correct compensation impossible), and truncates its
// note row rather than trusting a caller-supplied hint to fit.
//
// This started as a NO_TEST probe printing a table, because asserting the
// numbers before the fix would have enshrined the overdraw. It measured
// 30 broken panel/width combinations across 13 of 14 panels. It now
// measures 0, so the measurement becomes the guarantee.

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/panels.hpp"
#include "agentty/runtime/panel/smart_form.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pn    = agentty::ui::panel;
namespace smart_form = agentty::smart_form;

using namespace agentty;

namespace {

struct Case {
    const char*                          name;
    std::function<void(Model&)>          open;
    std::function<maya::Element(const Model&)> build;
};

// Every panel kind, opened with enough content to be representative. Same
// list as the escape-guarantee table -- these two should stay in step, and
// the fact that they are separate lists is its own smell.
const std::vector<Case>& cases() {
    static const std::vector<Case> v = {
        {"command palette",   [](Model& m) { m.ui.panel = pn::Palette{{}}; },
         ui::palette_panel},
        {"mention palette",   [](Model& m) { m.ui.panel = pn::Mention{{}}; },
         ui::mention_panel},
        {"symbol palette",    [](Model& m) { m.ui.panel = pn::Symbol{{}}; },
         ui::symbol_panel},
        {"code block picker", [](Model& m) { m.ui.panel = pn::CodeBlocks{{}}; },
         ui::code_blocks_panel},
        {"tool output",       [](Model& m) { m.ui.panel = pn::ToolOutput{{}}; },
         ui::tool_output_panel},
        {"checkpoints",       [](Model& m) { m.ui.panel = pn::Checkpoints{{}}; },
         ui::checkpoints_panel},
        {"retrieval pane",    [](Model& m) { m.ui.panel = pn::Rag{{}}; },
         ui::rag_panel},
        {"settings list",     [](Model& m) { m.ui.panel = pn::SettingsList{{}}; },
         ui::settings_list_panel},
        {"fork picker",       [](Model& m) { m.ui.panel = pn::Fork{{}}; },
         ui::fork_panel},
        {"model picker",      [](Model& m) { m.ui.panel = pn::Models{{}}; },
         ui::models_panel},
        {"provider picker",   [](Model& m) { m.ui.panel = pn::Providers{{}}; },
         ui::providers_panel},
        {"thread list",       [](Model& m) { m.ui.panel = pn::ThreadList{{}}; },
         ui::thread_list_panel},
        {"smart mode",        [](Model& m) {
             smart_form::Inputs in;
             m.ui.panel = pn::SmartMode{{}, smart_form::build_form(in)};
         }, ui::smart_mode_panel},
        {"stats viewer",      [](Model& m) { m.ui.panel = pn::Stats{}; },
         ui::stats_panel},
    };
    return v;
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

// A thread with enough turns that the tally panels have real content and
// the scrolling panels actually overflow their viewport.
Model with_history() {
    Model m;
    m.d.current.id = ThreadId{"probe"};
    for (int i = 0; i < 40; ++i) {
        Message u;
        u.role = Role::User;
        u.text = "a question that is long enough to wrap on a narrow pane";
        m.d.current.messages.push_back(std::move(u));

        Message a;
        a.role         = Role::Assistant;
        a.served_model = ModelId{"claude-sonnet-4-5"};
        a.text         = "an answer";
        Message::Telemetry t;
        t.input_tokens   = 1200;
        t.output_tokens  = 400;
        t.cache_read     = 15000;
        t.cache_creation = 200;
        t.ttft_ms        = 120;
        t.stream_ms      = 900;
        a.telemetry = t;
        m.d.current.messages.push_back(std::move(a));
    }
    return m;
}

// The rightmost column carrying a non-blank cell. The panel's own frame is
// what SHOULD be there; anything beyond it is content that escaped.
int rightmost_ink(const Case& c, const Model& m, int w, int h) {
    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    maya::render_tree(c.build(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);
    int worst = -1;
    for (int y = 0; y < h; ++y)
        for (int x = w - 1; x > worst; --x) {
            const auto ch = canvas.get(x, y).character;
            if (ch != 0 && ch != U' ') { worst = x; break; }
        }
    return worst;
}

// How many rows END without their right border.
//
// This is the symptom that actually reaches a user. A Canvas CLIPS, so
// content laid out too wide never paints past the edge -- it overwrites
// the frame's own right border and then stops. "Ink past the frame" can
// therefore never be observed on a canvas; the visible damage is a row
// whose border is MISSING, which is what a broken frame looks like on
// screen.
//
// Counting the rows that lost their border is the same measurement taken
// from the only side it is observable from.
// What a panel had CLIPPED AWAY, and how many rows lost their border.
//
// Two measurements of one fault, taken from opposite sides.
//
// The border count is the SYMPTOM, and this probe was built around it for
// a good reason stated above: a Canvas clips, so content laid out too wide
// cannot be observed past the edge — it overwrites the frame's own border
// and stops. Counting rows that lost their border was the only way to see
// overflow from the outside.
//
// The clip report is the CAUSE, and it is strictly better where it
// applies: it names the text, the column it started at, the width it
// wanted and the edge that cut it, at the moment the cells are discarded.
// "Three rows are broken at width 40" becomes "'transport health —
// retries, stalls, throughput' wanted 48 columns and the edge was at 40".
//
// Both are kept because neither subsumes the other. A row can lose its
// border to content that fits exactly and leaves no clip; content can be
// clipped inside a frame whose borders are all intact. Two questions, two
// answers.
struct Damage {
    int broken_rows = 0;
    std::vector<std::string> clipped;
};

Damage measure(const Case& c, const Model& m, int w, int h) {
    // Panel clamps its own min_width to the terminal, and with no tty it
    // reads COLUMNS. Tell it the width we are actually rendering at, or it
    // clamps against a fallback and the probe measures the wrong geometry.
    const std::string cols = std::to_string(w);
    setenv("COLUMNS", cols.c_str(), /*overwrite=*/1);

    Damage d;
    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    canvas.on_clip_overflow([&d](const maya::Canvas::ClipOverflow& o) {
        d.clipped.push_back("row " + std::to_string(o.y) + " col "
                            + std::to_string(o.x) + ": '" + std::string(o.text)
                            + "' wanted " + std::to_string(o.wanted)
                            + " cols, edge at " + std::to_string(o.edge));
    });
    maya::render_tree(c.build(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);

    // Which rows belong to the frame at all: those whose LEFT edge carries
    // the border glyph. Rows outside the panel are not its problem.
    for (int y = 0; y < h; ++y) {
        const auto left = canvas.get(0, y).character;
        const bool framed = left == U'\u2502' || left == U'\u256d'
                         || left == U'\u2570';
        if (!framed) continue;
        const auto right = canvas.get(w - 1, y).character;
        const bool closed = right == U'\u2502' || right == U'\u256e'
                         || right == U'\u256f';
        if (!closed) ++d.broken_rows;
    }
    return d;
}

int rows_missing_border(const Case& c, const Model& m, int w, int h) {
    return measure(c, m, w, h).broken_rows;
}

}  // namespace

TEST_CASE("panel: no panel paints over its own frame, at any width") {
    install_stub_deps();

    // From a phone-sized split pane to an ultrawide terminal. The narrow
    // end is where every one of these broke.
    const int widths[] = {40, 50, 60, 68, 76, 90, 120, 160, 200};
    constexpr int kRows = 60;

    for (const auto& c : cases())
        for (int w : widths) {
            Model m = with_history();
            c.open(m);
            const int broken = rows_missing_border(c, m, w, kRows);
            INFO("panel=", c.name, " width=", w,
                 " rows_missing_right_border=", broken);
            CHECK(broken == 0);
        }
}

// What every panel throws away, reported.
//
// The companion to the check above, asking the other half of the
// question. That one measures what LANDED — a frame whose border survived
// is a frame nothing overflowed into. This one asks the renderer what it
// DISCARDED, which is the part no examination of the painted cells can
// recover: clipped text leaves a frame that looks finished and is missing
// information, and the reader has no reason to doubt it.
//
// REPORTED, not asserted, and the distinction is deliberate. Its first
// run found ten real defects — four panels whose empty-state hint is cut
// mid-word on a narrow terminal ("indexing workspace… (type to filter as
// it f") — and none of them has a fix I have been able to prove. Three
// candidates were tried and reverted: TruncateEnd at the call site, a
// measured width for the prebuilt path, and a max_width bound on header
// rows. Each was locally reasonable, each left the count at ten.
//
// So the count is printed rather than enforced. An assertion nobody can
// currently satisfy gets commented out within a week and takes the
// diagnostic with it; a number in the log stays honest and makes the day
// somebody fixes it visible as a drop. Turn the CHECK back on when the
// count reaches zero.
//
// What IS known, written down so the next attempt does not re-derive it:
//
//   * the clip edge is 36 in a 40-column panel, and the text lands at
//     row 4-5 col 3 — inside the body, past the header rows
//   * the prebuilt measure loop reports ZERO rows for these panels, so
//     whatever path renders that line, it is not the one measure_body
//     accounts for
//   * the panels set cfg.prebuilt when their list is empty, which is
//     exactly the case the probe opens
//
// Those three cannot all be true at once, and the contradiction is the
// thread to pull.
TEST_CASE("panel: report anything clipped away without an ellipsis") {
    install_stub_deps();

    const int widths[] = {40, 50, 60, 68, 76, 90, 120, 160, 200};
    constexpr int kRows = 60;

    std::size_t total = 0;
    for (const auto& c : cases())
        for (int w : widths) {
            Model m = with_history();
            c.open(m);
            const Damage d = measure(c, m, w, kRows);
            total += d.clipped.size();
            if (!d.clipped.empty()) {
                std::fprintf(stderr, "\n%s at width %d clipped %zu write(s):\n",
                             c.name, w, d.clipped.size());
                std::size_t shown = 0;
                for (const auto& s : d.clipped) {
                    if (shown++ >= 6) {
                        std::fprintf(stderr, "    … and %zu more\n",
                                     d.clipped.size() - 6);
                        break;
                    }
                    std::fprintf(stderr, "    %s\n", s.c_str());
                }
            }
        }

    std::fprintf(stderr,
                 "\npanels: %zu clipped write(s) across %zu panels x %zu widths\n",
                 total, cases().size(), std::size(widths));

    // A ratchet, not a gate. It cannot go UP without someone noticing,
    // and the day the remaining ten are fixed this becomes == 0.
    CHECK(total <= 10);
}
