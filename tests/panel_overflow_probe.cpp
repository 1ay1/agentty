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
int rows_missing_border(const Case& c, const Model& m, int w, int h) {
    // Panel clamps its own min_width to the terminal, and with no tty it
    // reads COLUMNS. Tell it the width we are actually rendering at, or it
    // clamps against a fallback and the probe measures the wrong geometry.
    const std::string cols = std::to_string(w);
    setenv("COLUMNS", cols.c_str(), /*overwrite=*/1);

    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    maya::render_tree(c.build(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);

    // Which rows belong to the frame at all: those whose LEFT edge carries
    // the border glyph. Rows outside the panel are not its problem.
    int broken = 0;
    for (int y = 0; y < h; ++y) {
        const auto left = canvas.get(0, y).character;
        const bool framed = left == U'\u2502' || left == U'\u256d'
                         || left == U'\u2570';
        if (!framed) continue;
        const auto right = canvas.get(w - 1, y).character;
        const bool closed = right == U'\u2502' || right == U'\u256e'
                         || right == U'\u256f';
        if (!closed) ++broken;
    }
    return broken;
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
