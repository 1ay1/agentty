#include "agentty/runtime/view/view.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include <maya/core/render_context.hpp>
#include <maya/element/builder.hpp>
#include <maya/platform/io.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/widget/app_layout.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/overlay.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/runtime/view/login.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/pickers.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/thread.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

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
        // grow-spacer, and flush-left so live turns byte-align with the
        // bare sealed nodes — or a blank void / column shift appears at
        // the freeze seam.
        //
        // EXCEPTION: when a modal overlay (picker / palette / login) is
        // open, force FILL mode even on the strata path. A picker is a
        // full-screen modal floated over the base via maya::Overlay,
        // which z-stacks the float over the base and SIZES TO THE BASE.
        // A hugged base is only a few rows tall, so a 15-row picker would
        // overflow/clip and shove the composer around ("pickers mess up
        // the layout"). Flooring the base to the viewport gives the
        // overlay a full-height canvas to anchor against — the picker
        // floats correctly and the sealed turns scroll above it, exactly
        // as on the classic path. There is no live streaming turn whose
        // geometry must stay seam-stable while a modal owns the screen,
        // so the temporary fill is invisible to the user.
        const bool overlay_present = overlay.has_value();
        alc.fill_viewport = include_frozen || overlay_present;
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

// Forward declaration — defined below the anonymous helpers it needs.
maya::Element build_settled_run(const Model& m, std::size_t run_first);

// ── Settled-run builder (shared by Strata's lazy build path) ─────────
//
// Builds ONE settled speaker-run [run_first, run_end) as a Turn Element,
// preceded by the inter-turn gap (and a compaction divider when the run
// opens on a compaction boundary) so a sealed node is byte-identical to
// how the same run rendered in the live tail the frame before it sealed.
// This is the lazy replacement for the old freeze_range push loop: maya
// calls it only on a cache miss for an on-screen settled node, never for
// a scrolled-off one, so there is no eager snapshot and no height math.
//
// The leading gap is omitted for the very first run in the transcript
// (run_first == 0) so the top of the thread has no orphan gap — matching
// build_live_tail's first_overall guard.
namespace {

maya::Element gap_row_v() {
    using namespace maya::dsl;
    return v(blank(),
             maya::Conversation::divider(),
             blank()).build();
}

maya::Element compaction_divider_row_v() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";   // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = muted;
    return maya::Turn{std::move(cfg)}.build();
}

bool run_opens_on_compaction(const Model& m, std::size_t idx) {
    const std::size_t total = m.d.current.messages.size();
    for (const auto& rec : m.d.current.compactions)
        if (rec.up_to_index == idx && rec.up_to_index > 0
            && rec.up_to_index <= total) return true;
    return false;
}

// Count of assistant runs strictly before `run_first` — the display turn
// number a settled run carries. Computed on demand (cheap: a few integer
// hops over run boundaries) instead of tracked in a persisted counter.
int assistant_runs_before(const Model& m, std::size_t run_first) {
    const auto& msgs = m.d.current.messages;
    int n = 0;
    for (std::size_t k = 0; k < run_first; ) {
        const std::size_t re = ui::turn_run_end(msgs, k);
        if (msgs[k].role == Role::Assistant) ++n;
        k = re;
    }
    return n;
}

} // namespace

maya::Element build_settled_run(const Model& m, std::size_t run_first) {
    using namespace maya::dsl;
    const auto& msgs = m.d.current.messages;
    const std::size_t total = msgs.size();
    if (run_first >= total) return maya::detail::nothing();

    const std::size_t run_end = ui::turn_run_end(msgs, run_first);

    // Settled runs render their tool bodies with full content (show_all),
    // unlike the live tail which elides to a window for per-frame cheapness.
    ui::FrozenBuildScope frozen_scope;

    std::vector<maya::Element> rows;
    rows.reserve(3);

    if (run_first > 0 && run_opens_on_compaction(m, run_first))
        rows.push_back(compaction_divider_row_v());
    if (run_first > 0)
        rows.push_back(gap_row_v());

    const Message& head = msgs[run_first];
    const int runs_before = assistant_runs_before(m, run_first);
    if (head.role == Role::Assistant) {
        const int turn_num = runs_before + 1;
        auto cfg = ui::turn_config_for_assistant_run(
            run_first, run_end, turn_num, m);
        cfg.hash_id = ui::assistant_run_hash_id(m, run_first, run_end);
        rows.push_back(maya::Turn{std::move(cfg)}.build());
    } else {
        // User / compaction-summary single-message run. Numbered with the
        // count of assistant runs settled so far (the preceding turn's
        // number), matching build_live_tail's turn_num - 1 policy.
        auto cfg = ui::turn_config(head, run_first, runs_before, m,
                                   /*continuation=*/false);
        cfg.hash_id = maya::CacheIdBuilder{}
            .add(std::string_view{"agentty.turn"})
            .add(std::string_view{head.id.value})
            .add(head.compute_render_key())
            .build();
        rows.push_back(maya::Turn{std::move(cfg)}.build());
    }

    if (rows.size() == 1) return std::move(rows.front());
    return v(rows).build();
}


// ── Strata builders (lazy depositional node model) ───────────────────
//
// The host keeps NO snapshot vector, NO frozen-height accounting, and NO
// settle-freeze timing. It enumerates the transcript as a flat list of
// LOGICAL TURN nodes computed fresh from m.d.current.messages each frame:
//
//   [ run_0, run_1, …, run_{k-1}, LIVE ]
//
// Each settled speaker-run (the same boundary build_live_tail / the old
// freeze_range used, via ui::turn_run_end) is ONE node keyed by its
// run-start message index. A run node is `terminal` — eligible for maya
// to seal into native scrollback — iff it is fully SETTLED *and* fully
// DRAINED of reveal animation (see run_is_sealable). Until a run drains,
// it stays non-terminal with a per-frame-bumping hash so Strata rebuilds
// it and the typewriter reveal animates; the instant it drains it gains a
// STABLE content hash (turn_run_key) and Strata caches the clean bytes.
// That single `terminal` bit subsumes the entire old
// pending_settle_freeze / settle_cooldown / reveal_settled ritual: the
// freeze instant is no longer a stateful cross-frame event the host
// schedules, it is a pure per-node predicate maya reads every frame.
//
// The trailing LIVE node is the in-flight run's live tail PLUS the bottom
// chrome (composer, status bar, changes strip) and any modal overlay —
// they must stay glued to the live turn so maya never seals the composer
// away. Its hash bumps every frame (live content + spinner + caret), so
// Strata always rebuilds it; everything above it that has drained is a
// cache hit. Per-frame cost is therefore O(live turn + chrome), flat in
// session length, with zero host bookkeeping.

// A speaker-run [run_first, run_end) is sealable when every message in it
// is settled (no streaming bytes, every tool terminal) AND every
// assistant message's reveal animation has fully drained (the widget
// flipped live_ off on its own, the finalize ramp + scramble settle
// completed, no async parse in flight). This is the EXACT predicate the
// old finalize→pending_settle_freeze→reveal_settled chain gated on,
// collapsed to a stateless per-run check. A run that fails it stays a
// non-terminal (rebuilt, animated) node instead of being sealed with
// stale scramble cells.
bool run_is_sealable(const Model& m, std::size_t run_first,
                     std::size_t run_end) {
    const auto& msgs = m.d.current.messages;
    for (std::size_t j = run_first; j < run_end && j < msgs.size(); ++j) {
        const Message& mm = msgs[j];
        if (!mm.streaming_text.empty() || !mm.pending_stream.empty())
            return false;
        for (const auto& tc : mm.tool_calls)
            if (!tc.is_terminal()) return false;
        if (mm.role == Role::Assistant && !mm.text.empty()) {
            const auto& mc = m.ui.view_cache.message_md(
                m.d.current.id, mm.id);
            if (mc.streaming
                && (mc.streaming->is_live()
                 || mc.streaming->is_finalizing()
                 || mc.streaming->reveal_in_progress()
                 || mc.streaming->is_parsing()))
                return false;
        }
    }
    return true;
}

// Stable content hash for a settled run node — folds the same message
// render keys assistant_run_hash_id mixes, so a sealed run's hash is
// invariant for the life of the entry (cache hit) yet changes if any
// underlying message content does (cache miss → rebuild). Used only for
// terminal nodes; the live run uses a bumping generation instead.
std::uint64_t turn_run_key(const Model& m, std::size_t run_first,
                           std::size_t run_end) {
    std::uint64_t k = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t v) { k = (k ^ v) * 1099511628211ULL; };
    const auto& msgs = m.d.current.messages;
    for (std::size_t j = run_first; j < run_end && j < msgs.size(); ++j) {
        for (char c : msgs[j].id.value)
            mix(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        mix(msgs[j].compute_render_key());
    }
    // Distinguish a run by its boundary so two structurally-identical
    // adjacent runs (rare, but possible with empty turns) keep distinct
    // keys and identities.
    mix(run_first * 1000003ULL + run_end);
    return k;
}

std::vector<maya::strata::NodeRef> strata_nodes(const Model& m) {
    // Monotonic per-process generation for the LIVE node — its content
    // (composer, streaming tail, spinner, overlay) changes constantly, so
    // bump every call to force a rebuild. The run loop only calls this
    // behind the visual_hash gate, so it never spins idle.
    static std::uint64_t live_gen = 0;
    ++live_gen;

    const auto& msgs  = m.d.current.messages;
    const std::size_t total = msgs.size();

    std::vector<maya::strata::NodeRef> ns;
    ns.reserve(total + 1);

    // Walk whole speaker-runs front to back. Every run BEFORE the live
    // boundary becomes its own node; the live boundary is the first run
    // that is either the last run in the transcript or not yet sealable —
    // from there on (the in-flight turn) everything folds into the single
    // LIVE chrome node so the composer stays glued to it.
    std::size_t i = 0;
    while (i < total) {
        const std::size_t run_end = ui::turn_run_end(msgs, i);
        const bool is_last     = (run_end >= total);
        const bool sealable    = !is_last && run_is_sealable(m, i, run_end);
        if (!sealable) break;   // live boundary — fold the rest into LIVE
        ns.push_back({static_cast<std::uint64_t>(i),
                      turn_run_key(m, i, run_end),
                      /*terminal=*/true});
        i = run_end;
    }

    // LIVE node: the live tail (runs [i, total)) + chrome + overlay,
    // keyed by the boundary index so its identity is stable across frames
    // (only its hash bumps). kStrataLiveKey distinguishes the chrome node
    // unambiguously from any run-start index in strata_build.
    ns.push_back({kStrataLiveKey, live_gen, /*terminal=*/false});
    // Stash the live boundary so strata_build / view_impl render exactly
    // the live runs (and conversation_config knows where the live tail
    // starts) without a persisted frozen_through cursor.
    m.ui.live_run_start = i;
    return ns;
}

// strata_build: LIVE key → the live tail + chrome + overlay (the
// monolithic view minus the settled runs); a run-start index → that
// settled run built lazily on demand. Strata invokes this only on a miss,
// and never for a sealed (scrolled-off) node, so the build cost is
// bounded by what is currently on screen.
maya::Element strata_build(const Model& m, std::uint64_t key) {
    if (key == kStrataLiveKey)
        return view_impl(m, /*include_frozen=*/false);
    if (key < m.d.current.messages.size())
        return build_settled_run(m, static_cast<std::size_t>(key));
    return maya::detail::nothing();   // defensive — out-of-range key
}

} // namespace agentty::ui
