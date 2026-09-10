// stats.cpp — the stats viewer reducer.
//
// Three arms, and that is the whole surface: open, step tabs, close. The
// panel is a projection of the transcript, so there is nothing to select,
// nothing to commit and nothing to persist — see domain/stats.hpp.
//
// The tally itself is NOT computed here. It is derived in the view from the
// live transcript (cached against a stamp), which keeps the reducer free of
// a snapshot that could go stale the moment the next turn lands.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/stats.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using namespace agentty::msg;

Step stats_update(Model m, msg::StatsMsg sm) {
    return std::visit(maya::overload{
        [&](OpenStats) -> Step {
            // descend(), not a bare open: the viewer is reachable from the
            // command palette, so Esc must land back where the user came
            // from rather than closing everything. The shared ascend()
            // wrapper handles that on the way out.
            pn::Stats pane{};
            m.ui.panel.descend(std::move(pane));
            return done(std::move(m));
        },

        [&](CloseStats) -> Step {
            ascend(m);
            return done(std::move(m));
        },

        [&](StatsTab e) -> Step {
            auto* o = m.ui.panel.get<pn::Stats>();
            if (!o) return done(std::move(m));
            // Stepping is total on the enum (stats::tab_step wraps), so a
            // new tab joins the cycle with no change here.
            o->tab = stats::tab_step(o->tab, e.delta);
            // The cached projection is per-transcript, not per-tab, so the
            // stamp stays valid across a tab switch — switching views costs
            // nothing.
            return done(std::move(m));
        },
    }, std::move(sm));
}

} // namespace agentty::app::detail
