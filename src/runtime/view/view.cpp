#include "agentty/runtime/view/view.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include <maya/core/render_context.hpp>
#include <maya/element/builder.hpp>
#include <maya/platform/io.hpp>
#include <maya/widget/app_layout.hpp>
#include <maya/widget/overlay.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/runtime/view/login.hpp"
#include "agentty/runtime/view/pickers.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

namespace agentty::ui {

namespace {

// Pick the active overlay, if any. Login modal has highest priority —
// auth gates everything else.
std::optional<maya::Element> pick_overlay(const Model& m) {
    if (login::is_open(m.ui.login))        return login_modal(m);
    if (pick::is_open(m.ui.model_picker))  return model_picker(m);
    if (pick::is_open(m.ui.provider_picker)) return provider_picker(m);
    if (pick::is_open(m.ui.thread_list))   return thread_list(m);
    if (is_open(m.ui.command_palette))     return command_palette(m);
    if (mention_is_open(m.ui.mention_palette)) return mention_palette(m);
    if (symbol_palette_is_open(m.ui.symbol_palette)) return symbol_palette(m);
    if (pick::is_open(m.ui.diff_review))   return diff_review(m);
    if (pick::is_open(m.ui.todo.open))     return todo_modal(m);
    return std::nullopt;
}

// Bottom-inset overlay compose. maya's Overlay widget bottom-pins the
// picker to the base BOX bottom and paints a full-width bg fill over
// its whole hugging rect. The base vstack's box is content_height + 2
// (the outer bottom-padding row + the idle anti-bounce blank()), so
// opening any picker painted 2 rows the closed frame never paints —
// frame grows +2 on open, shrinks -2 on close. When the welcome screen
// already sits at/over the terminal viewport, the +2 pushes the top
// rows into native scrollback (unreclaimable), and the close-shrink
// recovery (the bobbing wordmark fails the committed-prefix match)
// strands a wordmark slice EVERY open/close cycle — "the wordmark gets
// longer with every picker".
//
// Fix: pin the overlay 2 rows ABOVE the box bottom (inset bottom=2) so
// its painted extent never exceeds the base's painted extent — opening
// a picker can never change the frame height, so no rows cross the
// viewport boundary and nothing strands. maya::Overlay's Anchor +
// inset express exactly this (the inset sits OUTSIDE the bg-filled box,
// which a plain in-box padding can't do).
maya::Element compose_overlay(maya::Element base, maya::Element overlay) {
    return maya::Overlay{{
        .base    = std::move(base),
        .overlay = std::move(overlay),
        .present = true,
        .anchor  = maya::Overlay::Anchor::BottomCenter,
        .inset   = {0, 0, 2, 0},
    }}.build();
}

maya::Element view_impl(const Model& m, bool include_frozen) {
    // ── Terminal dimensions for the BUILD phase ──
    // maya's run loop calls P::view(model) BEFORE Runtime::render
    // installs the sized RenderContext (the only guard site), so any
    // available_height()/available_width() read during Element
    // construction would see the 24x80 DEFAULT. Parent ctx wins (a
    // nested render or a test harness driving simulated dims), else
    // one cheap ioctl, else COLUMNS/LINES (tests, pipes).
    int cols = 0, rows = 0;
    if (maya::detail::render_ctx_) {
        cols = maya::available_width();
        rows = maya::available_height();
    } else {
        const auto sz = maya::platform::query_terminal_size(
            maya::platform::stdout_handle());
        cols = sz.width.value;
        rows = sz.height.value;
        if (cols <= 0)
            if (const char* e = std::getenv("COLUMNS")) cols = std::atoi(e);
        if (rows <= 0)
            if (const char* e = std::getenv("LINES"))   rows = std::atoi(e);
    }
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;

    // ── Phase 1: configs + overlay under the REAL dimensions ──
    // The welcome clamp (welcome_screen_config) must see the true
    // terminal height to size its row budget.
    maya::AppLayout::Config alc;
    std::optional<maya::Element> overlay;
    {
        maya::RenderContext ctx{cols, rows, maya::render_generation(),
                                /*auto_height=*/true};
        maya::RenderContextGuard guard(ctx);
        alc.thread        = thread_config(m, include_frozen);
        alc.changes_strip = changes_strip_config(m);
        alc.composer      = composer_config(m);
        alc.status_bar    = status_bar_config(m);
        overlay = pick_overlay(m);
        // Strata LIVE node (include_frozen=false): HUG mode. The settled
        // turns are sealed nodes ABOVE this one, so the live node must
        // hug its own content — no viewport-fill floor, no thread
        // grow-spacer — or a blank void strands between the settled
        // turns and the composer.
        alc.fill_viewport = include_frozen;
    }

    // ── Phase 2: layout build under a HEIGHT-CAPPED context ──
    // AppLayout::build bakes min_height(fixed(available_height()))
    // into the base vstack. Historically P::view always ran under
    // maya's 24-row DEFAULT context (the sized one is installed only
    // inside Runtime::render, AFTER view returns), so that min_height
    // was a de-facto constant 24 in every working inline app
    // (agent_session included): on a tall terminal the base box HUGS
    // the content and the bottom-pinned picker floats just below the
    // status bar. Handing build the REAL height regressed that — on an
    // 80-row terminal the base box spanned the whole viewport, the
    // picker dropped to the terminal bottom behind a huge dead gap,
    // and opening it grew the painted frame from ~26 rows to the full
    // screen: a resize-class reflow on a mere overlay toggle. Cap at
    // min(rows, 24): tall terminals keep the historic content-hugged
    // box; terminals SHORTER than 24 get the real height so the box —
    // and anything pinned to its bottom — never overhangs the viewport.
    maya::RenderContext lctx{cols, std::min(rows, 24),
                             maya::render_generation(),
                             /*auto_height=*/true};
    maya::RenderContextGuard lguard(lctx);

    auto base = maya::AppLayout{std::move(alc)}.build();
    if (!overlay) return base;
    return compose_overlay(std::move(base), std::move(*overlay));
}

} // namespace

maya::Element view(const Model& m) {
    // Classic monolithic path (tests, fallback): full tree incl. the
    // borrowed frozen prefix.
    return view_impl(m, /*include_frozen=*/true);
}

// ── Strata builders ──────────────────────────────────────────────────
//
// strata_nodes: [ frozen[0], …, frozen[n-1], LIVE ]. Each settled
// m.ui.frozen Element is one terminal node keyed by its stable index
// (immutable once pushed — the host appends, never reorders); the live
// tail + chrome + overlay is one non-terminal LIVE node keyed
// kStrataLiveKey whose hash bumps every frame so maya rebuilds it.
std::vector<maya::strata::NodeRef> strata_nodes(const Model& m) {
    // Monotonic per-process generation — the LIVE node's content changes
    // constantly (composer, streaming tail, spinner, overlay), so bump
    // every call to force a rebuild. The run loop only calls this when
    // something changed (visual_hash gate), so it never spins idle.
    static std::uint64_t live_gen = 0;
    ++live_gen;

    std::vector<maya::strata::NodeRef> ns;
    ns.reserve(m.ui.frozen.size() + 1);
    for (std::size_t i = 0; i < m.ui.frozen.size(); ++i)
        ns.push_back({static_cast<std::uint64_t>(i),
                      static_cast<std::uint64_t>(i),   // immutable once sealed
                      /*terminal=*/true});
    ns.push_back({kStrataLiveKey, live_gen, /*terminal=*/false});
    return ns;
}

// strata_build: LIVE → the live tail + chrome + overlay (the monolithic
// view minus the frozen prefix); a frozen index → the cached settled
// Element. Never invoked for a sealed node, so the index is always live.
maya::Element strata_build(const Model& m, std::uint64_t key) {
    if (key == kStrataLiveKey)
        return view_impl(m, /*include_frozen=*/false);
    if (key < m.ui.frozen.size())
        return m.ui.frozen[static_cast<std::size_t>(key)];
    return maya::detail::nothing();   // defensive — maya only builds live keys
}

} // namespace agentty::ui
