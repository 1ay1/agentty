// stats_render_probe — does the stats panel actually draw?
//
// The unit tests cover the projection (numbers) and the strip (geometry).
// Neither renders the PANEL, so a host that wires the two together wrongly —
// a tab index off by one, an empty body, a strip that never appears — passes
// both and still shows a broken pane. This drives the real view function and
// prints what a user would see.

#include "agtest.hpp"

#include "agentty/domain/stats.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/stats.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

Message served(const char* model, smart::ModelRole role) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    m.served_role  = role;
    return m;
}

std::vector<std::string> render_rows(const Model& m, int w, int h) {
    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    maya::render_tree(ui::stats_panel(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);
    std::vector<std::string> rows;
    for (int y = 0; y < h; ++y) {
        std::string s;
        for (int x = 0; x < w; ++x) {
            const auto c = canvas.get(x, y);
            if (c.character >= 0x20 && c.character < 0x7F)
                s += static_cast<char>(c.character);
            else if (c.character == 0 || c.character == U' ') s += ' ';
            else s += '?';
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        rows.push_back(std::move(s));
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    return rows;
}

bool any_row_has(const std::vector<std::string>& rows, std::string_view n) {
    for (const auto& r : rows)
        if (r.find(n) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("stats panel: renders a real thread") {
    Model m;
    m.d.current.messages = {
        served("claude-opus-4-5",   smart::ModelRole::Strategic),
        served("claude-opus-4-5",   smart::ModelRole::Strategic),
        served("claude-sonnet-4-5", smart::ModelRole::Implementation),
        served("claude-haiku-4-5",  smart::ModelRole::Utility),
        served("claude-haiku-4-5",  smart::ModelRole::Utility),
    };
    m.ui.panel.descend(pn::Stats{});

    const auto rows = render_rows(m, 76, 40);
    std::printf("\n--- stats panel (5 turns) ---\n");
    for (const auto& r : rows) std::printf("|%s\n", r.c_str());

    CHECK(!rows.empty());                       // it drew something
    CHECK(any_row_has(rows, "Stats"));          // the frame title
    CHECK(any_row_has(rows, "Smart Mode"));     // the tab strip
    CHECK(any_row_has(rows, "BY ROLE"));
    CHECK(any_row_has(rows, "BY MODEL"));
    CHECK(any_row_has(rows, "Strategic"));
    CHECK(any_row_has(rows, "Utility"));
    // 3 of 5 routed turns ran below Strategic.
    CHECK(any_row_has(rows, "60%"));
    CHECK(any_row_has(rows, "5 routed"));       // the denominator is stated
}

TEST_CASE("stats panel: the empty state is not a wall of zeroes") {
    Model m;
    m.ui.panel.descend(pn::Stats{});
    const auto rows = render_rows(m, 76, 24);
    std::printf("\n--- stats panel (empty) ---\n");
    for (const auto& r : rows) std::printf("|%s\n", r.c_str());

    CHECK(!rows.empty());
    CHECK(any_row_has(rows, "No turns"));
    CHECK(!any_row_has(rows, "BY ROLE"), "no tally when there is nothing");
}

TEST_CASE("stats panel: survives a narrow terminal") {
    // A panel is a view: it must degrade, never abort.
    Model m;
    m.d.current.messages = {served("m", smart::ModelRole::Utility)};
    m.ui.panel.descend(pn::Stats{});
    for (int w : {20, 30, 40, 60, 100}) {
        const auto rows = render_rows(m, w, 30);
        CHECK_MESSAGE(!rows.empty(), "renders at width " << w);
    }
}
