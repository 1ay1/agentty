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
            // Open at the top. The panel state is fresh on every open (a
            // new pn::Stats, so a new projection) but the SCROLL is not:
            // it lives on Model::UI and outlives the panel, so without
            // this a reopen restored the offset from the last time the
            // viewer was closed — against a different tab, a different
            // transcript length, and a max_y describing neither.
            m.ui.stats_scroll.scroll_to_origin();
            return done(std::move(m));
        },

        [&](CloseStats) -> Step {
            ascend(m);
            return done(std::move(m));
        },

        [&](StatsTab e) -> Step {
            auto* o = m.ui.panel.get<pn::Stats>();
            if (!o) return done(std::move(m));
            // Step within the VISIBLE set, so Tab never lands on a view
            // the strip is not drawing. That needs the Facts, because
            // availability is a question about the data — refresh() is
            // the same amortised-O(1) call the view makes, and the
            // projection is shared, so this costs nothing extra.
            const auto& f = o->projection.refresh(m.d.current);
            o->tab = stats::tab_step(o->tab, e.delta, f);
            // A new tab starts at the top. Carrying the previous tab's
            // offset means switching from a tall tab to a short one opens
            // it scrolled past its own content — the user sees a blank
            // body and no reason for it.
            //
            // max_y is deliberately NOT reset with it. That field is owned
            // by the renderer's writeback, so the reducer has no honest
            // value to put there — the new tab's height is not known until
            // it is measured. It stays stale for exactly one frame, which
            // bounds a same-batch scroll to the old tab's range instead of
            // the new one's; the paint then clamps it. Zeroing it here to
            // "be safe" would swallow that keystroke entirely, which is a
            // worse trade than a transient the user cannot see.
            m.ui.stats_scroll.scroll_to_origin();
            // The projection is per-transcript, not per-tab, so switching
            // views re-folds nothing.
            return done(std::move(m));
        },
        [&](StatsScroll e) -> Step {
            // Scroll the body directly. scroll_by() clamps against BOTH
            // ends, so a delta past either settles at the edge rather than
            // scrolling into blank rows — which is what the ±1000000 that
            // nav sends for Home/End relies on.
            //
            // Through ScrollState rather than `y += delta` by hand. The
            // hand-rolled version guarded only the lower bound, so End
            // stored y = 1000000 and the model carried a value its own
            // invariant forbids until the next paint clamped it. The panel
            // does re-clamp on render, which is why nothing looked broken
            // — but that makes the model's correctness depend on a frame
            // happening, and any reducer reading y before then (a
            // same-batch StatsScroll after a StatsTab, and a fast terminal
            // delivers several keys in ONE read) reads a value that was
            // never legal.
            //
            // max_y is still last frame's here: it is written by the
            // renderer's writeback, so on the very first scroll after a
            // tab switch it describes the PREVIOUS tab. Clamping against a
            // stale bound is bounded and self-correcting; not clamping at
            // all is not. StatsTab resets y to 0 for the same reason.
            m.ui.stats_scroll.scroll_by(0, e.delta);
            return done(std::move(m));
        },
    }, std::move(sm));
}

} // namespace agentty::app::detail
