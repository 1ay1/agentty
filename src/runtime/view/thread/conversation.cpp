// conversation.cpp — view adapter for the conversation viewport.
//
// Strata depositional model: this builder emits ONLY the live tail
// (runs at/after m.ui.live_run_start — the in-flight assistant turn plus
// any queued-message previews). Settled runs above the live boundary are
// handed to maya as separate sealed nodes (built lazily by
// build_settled_run in view.cpp), which maya measures, caches, and seals
// into native scrollback itself.
//
// The per-frame cost is therefore O(visible_live_tail) regardless of how
// long the session has run. Settled runs are NEVER rebuilt here: maya
// keeps their cells and only re-invokes build_settled_run on a cache
// miss for an on-screen settled run. The host carries no Element
// snapshot vector and no freeze bookkeeping — just the live boundary.

#include "agentty/runtime/view/thread/conversation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/widget/activity_indicator.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/permission.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/activity_indicator.hpp"
#include "agentty/runtime/view/thread/turn/permission.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

namespace {

// Inter-turn seam between every pair of adjacent turns — a blank row,
// the dim ─ rule, then another blank row. MUST stay byte-identical to
// frozen.cpp's gap_row(): the same seam is pushed before each settled
// turn, between live-tail turns, and at the frozen↔live boundary. Any
// height/shape delta at a freeze instant would shift rows already
// scrolled into native scrollback against the live re-layout,
// producing a ghost at the scrollback↔viewport seam.
maya::Element gap_row() {
    using namespace maya::dsl;
    return v(blank(),
             maya::Conversation::divider(),
             blank()).build();
}

// Compaction-boundary divider. MUST stay byte-identical to frozen.cpp's
// compaction_divider_row(): freeze_range pushes this single-row
// `≡ Conversation compacted` rule before a run that begins on a
// compaction boundary, so the live tail has to push it too. Without the
// match the divider materialises only at freeze time — a +1 row
// shift that strands the pre-shift copy of the just-frozen turn in
// native scrollback (the post-compaction duplicate-turn / clipped-panel
// bug). See INLINE_SCROLLBACK.md pin #3 (divider symmetry).
maya::Element compaction_divider_row() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";   // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = muted;
    return maya::Turn{std::move(cfg)}.build();
}

// Sentinel-check: assistant message whose only content is tool_calls
// (no prose). Kept for any future per-message classification; the
// run-merge logic that previously used it now lives in the shared
// `ui::turn_run_end` / `ui::turn_config_for_assistant_run` helpers.
[[maybe_unused]] bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Build the live-tail Elements. One Turn per speaker-run: a User
// message is its own Turn; a run of consecutive Assistant messages
// (one logical agent turn, possibly split across N sub-turns by
// post-tool continuations) collapses into ONE Turn whose body
// interleaves each sub-turn's text and tool batch in source order.
// This is the agent_session shape — same merge logic the frozen
// builder uses (`freeze_range` calls the same `turn_run_end` /
// `turn_config_for_assistant_run` helpers), so the live and frozen
// row sequences are byte-identical for the same input.
void build_live_tail_from(const Model& m, std::size_t start,
                          int& running_turn,
                          std::vector<maya::Element>& out,
                          bool allow_continuation) {
    const std::size_t total = m.d.current.messages.size();
    start = std::min(start, total);
    if (start >= total) return;

    out.reserve(out.size() + (total - start) * 2);

    // Mirror freeze_range's needs_compaction_divider exactly: a run that
    // begins on a compaction boundary gets the `≡ Conversation
    // compacted` divider pushed before it. The frozen builder does this;
    // the live tail must match byte-for-byte or the divider only appears
    // after the freeze, shifting the just-frozen turn down one row and
    // duplicating it into native scrollback (INLINE_SCROLLBACK.md pin #3).
    auto compaction_boundary_at = [&](std::size_t idx) {
        for (const auto& rec : m.d.current.compactions) {
            if (rec.up_to_index == idx && rec.up_to_index > 0
                && rec.up_to_index <= total) return true;
        }
        return false;
    };

    bool first_in_tail = true;
    std::size_t i = start;
    while (i < total) {
        std::size_t run_end = turn_run_end(m.d.current.messages, i);

        // No mid-stream prefix-split and no mid-run continuation. The
        // whole live run renders as ONE Turn (matching agent_session,
        // where the entire in-flight assistant body lives in
        // m.assistant_body and only commits to frozen at MessageStop).
        // The carve machinery that needed a "continuation" remainder
        // (freeze_settled_subturns / freeze_streaming_text_prefix) has
        // been deleted — finalize_turn is the only freeze site, so the
        // live tail always starts at a whole-turn boundary.

        // Compaction divider FIRST (before the inter-turn gap), exactly
        // as freeze_range orders it, so the live and frozen row
        // sequences stay byte-identical across the freeze seam.
        //
        // The continuation first-run is the EXCEPTION: it is the tail of
        // an in-flight run whose settled head was split into a separate
        // sealed node. It must flow straight out of that head rail with
        // NO inter-rail gap and NO compaction divider (mid-run can't open
        // on a compaction boundary) — the one-row seam blank lives inside
        // the continuation rail (lead_gap) instead.
        const bool is_split_continuation =
            allow_continuation
            && m.ui.live_run_is_continuation && first_in_tail
            && i == m.ui.live_run_start
            && i < m.d.current.messages.size()
            && m.d.current.messages[i].role == Role::Assistant;

        if (!is_split_continuation && compaction_boundary_at(i)) {
            out.push_back(compaction_divider_row());
        }

        const bool first_overall = (start == 0) && first_in_tail && i == 0;
        if (!first_overall && !is_split_continuation) {
            out.push_back(gap_row());
        }
        first_in_tail = false;

        const Message& head = m.d.current.messages[i];
        int turn_num = running_turn;

        if (head.role == Role::Assistant) {
            // ── In-turn activity indicator (agent_session pattern).
            //    Show the breathing "thinking…" row whenever the agent
            //    is active and the assistant Turn has no body slots
            //    yet — i.e. cfg.body is empty after
            //    turn_config_for_assistant_run. Same shape as
            //    agent_session: thinking widget appears in the
            //    assistant Turn body until the first text/tool/etc.
            //    slot lands, then content replaces it.
            auto cfg = turn_config_for_assistant_run(
                i, run_end, turn_num, m,
                /*continuation=*/is_split_continuation,
                /*lead_gap=*/is_split_continuation);
            // Reserve an indicator-height slot for the WHOLE active
            // phase. When the tail is an empty placeholder we paint
            // the breathing "thinking…" widget; once real content
            // arrives we swap to a same-height invisible spacer so
            // the live-tail row count stays constant. Without this,
            // first-byte / first-tool flips the slot from 2 rows to 0,
            // Thread's trailing spacer can't absorb the shrink when
            // the transcript already fills the viewport, and Composer
            // jumps up by the indicator's height.
            const Message& tail = m.d.current.messages[run_end - 1];
            // "Empty placeholder" == nothing VISIBLE yet. Deliberately
            // does NOT require pending_stream to be empty: the first
            // content_block_delta lands its bytes in pending_stream
            // (stream.cpp) and they only become visible one Tick later
            // when meta.cpp drips them into streaming_text. If we
            // dropped the placeholder the instant pending_stream filled,
            // the indicator row (1 row) would vanish for that one frame
            // BEFORE cached_markdown_for has any streaming_text to draw
            // (has_text gates on text/streaming_text only) — a 1→0→1 row
            // blink at the live seam that pushes the composer/status bar
            // up for a split second at stream start. Holding the
            // indicator until streaming_text actually has bytes makes
            // the indicator→content swap a single height-stable step.
            const bool tail_is_empty_placeholder =
                tail.role == Role::Assistant
                && tail.text.empty()
                && tail.streaming_text.empty()
                && tail.tool_calls.empty();
            // Only the LAST run in the tail is the in-flight one whose
            // height must stay reserved across the indicator↔content
            // flip. Earlier runs in the tail are already settled (the
            // model moved on to a new sub-turn) — giving them a spacer
            // both wastes 2 rows and, more importantly, makes their
            // body shape differ from what freeze_range will build,
            // which would prevent the hash_id cache below from engaging
            // and force a full repaint of their (possibly huge) bodies
            // every frame for the whole duration of the active run.
            const bool is_last_run     = (run_end >= total);
            const bool reserve_slot   = m.s.active() && is_last_run;
            const bool show_indicator = reserve_slot && tail_is_empty_placeholder;
            if (show_indicator) {
                using namespace maya::dsl;
                maya::ActivityIndicator::Config ind;
                ind.edge_color    = cfg.rail_color;
                ind.spinner_glyph = std::string{m.s.spinner.current_frame()};
                ind.label         = "thinking";
                ind.words         = activity_indicator_words();

                if (const auto* a = active_ctx(m.s.phase)) {
                    ind.stream_bytes = a->live_delta_bytes;
                    if (a->first_delta_at.time_since_epoch().count() != 0) {
                        auto now = std::chrono::steady_clock::now();
                        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a->first_delta_at).count();
                        if (ts_ms >= 250) {
                            double sec = static_cast<double>(ts_ms) / 1000.0;
                            double tok = static_cast<double>(a->live_delta_bytes) / 4.0;
                            ind.stream_rate = static_cast<float>(tok / sec);
                        }
                    }
                }

                constexpr std::size_t kEntropyTail = 512;
                const std::string& a_text = head.streaming_text;
                const std::string& a_pend = head.pending_stream;
                const std::string& src = !a_pend.empty() ? a_pend : a_text;
                if (!src.empty()) {
                    std::size_t n = std::min(kEntropyTail, src.size());
                    ind.entropy_window = std::string_view{
                        src.data() + src.size() - n, n};
                }

                cfg.body.emplace_back(
                    maya::ActivityIndicator{std::move(ind)}.build());
            }
            // NOTE: no trailing spacer once real content exists. The
            // indicator occupies the body ONLY while the tail is an
            // empty placeholder; the moment the first text/tool slot
            // lands, the content itself holds the body height. Reserving
            // a 2-row spacer for the whole active phase (the old
            // behaviour) meant settled content carried dead trailing
            // space that VANISHED at turn-end — a visible 2-row collapse
            // the instant the run finished and the spacer dropped. The
            // indicator→first-content flip it was meant to smooth is a
            // non-event in practice: the first content slot is ≥1 row,
            // so the height step is small and happens once, mid-stream,
            // rather than as a jolt at every turn boundary.

            // Cache the settled-but-not-yet-sealed run. On the classic /
            // test path (live_start == 0) the WHOLE transcript is rebuilt
            // as the live tail, so without a hash_id maya would repaint
            // every settled run's body — including multi-thousand-line
            // write/edit cards — every frame. Stamping the stable
            // assistant_run_hash_id lets maya paint each settled run once
            // and blit thereafter. On the Strata path this also covers the
            // single frame between a run draining and strata_nodes
            // promoting it to a sealed terminal node: the SAME key is
            // reused there, so the promotion is a pure cache hit.
            //
            // run_is_sealable gates the stamp: it is false while any tool
            // is non-terminal OR any reveal overlay is still animating.
            // The reveal guard is CRITICAL — assistant_run_hash_id is keyed
            // on text content/size, INVARIANT across the reveal's
            // scramble→clean transition. Stamping mid-scramble would let
            // maya cache the scramble cells under a key that never changes,
            // stranding garbage glyphs on the settled tail forever. Leaving
            // an undrained run UNKEYED makes it rebuild + repaint every
            // frame (like the in-flight run) until the clean cells are what
            // gets cached.
            const bool sealable = run_is_sealable(m, i, run_end);
            if (sealable && !reserve_slot) {
                cfg.hash_id = assistant_run_hash_id(m, i, run_end);
            }
            // NOTE: the in-flight (streaming) run is deliberately NOT
            // cached. Its Turn carries animated chrome — the tool
            // spinner glyph and the live `elapsed` counter — which
            // change every frame independent of the body bytes. A
            // whole-Turn hash_id keyed on tool status + body-size
            // buckets would blit a stale card between buckets, freezing
            // the spinner and the elapsed readout ("liveness gone").
            // The per-frame rebuild is cheap because the streaming
            // write/edit body is sliced to a tail window in
            // tool_body_preview_config (O(window), not O(file)), so we
            // pay ~0.04ms to rebuild and keep the animation alive.
            out.push_back(maya::Turn{std::move(cfg)}.build());
            ++running_turn;
            i = run_end;
        } else {
            // User (or other non-Assistant) head: single-message Turn.
            // A user row carries the number of the assistant turn that
            // PRECEDED it (running_turn - 1), not the next assistant
            // number. running_turn tracks the NEXT assistant run, seeded
            // from the count of runs already settled before live_start, so
            // the matching user number here is running_turn - 1. This is
            // the SAME numbering build_settled_run uses for a settled user
            // run (assistant_runs_before), so a user turn renders
            // byte-identically whether it sits in the live tail or has been
            // sealed — no row shift at the seal boundary, no duplication.
            auto cfg = turn_config(head, i, turn_num - 1, m,
                                   /*continuation=*/false);
            out.push_back(maya::Turn{std::move(cfg)}.build());
            // User turns do not bump running_turn — the running count is
            // over Assistant turns only.
            i = run_end;
        }
    }
}

// Build the queued-message preview rows: visible at the tail of the
// transcript so the user can see what's queued. Mirrors Claude
// Code's appearance at offset 80106500 — visually identical to real
// user turns; the "queued not sent" cue is absence-of-assistant +
// the composer's `❚ N queued` chip.
void build_queued_previews(const Model& m, int& running_turn,
                           std::vector<maya::Element>& out) {
    if (m.ui.composer.queued.empty()) return;
    out.reserve(out.size() + m.ui.composer.queued.size() * 2);
    auto now = std::chrono::system_clock::now();
    const std::size_t base_idx = m.d.current.messages.size();
    for (std::size_t qi = 0; qi < m.ui.composer.queued.size(); ++qi) {
        Message synthetic;
        synthetic.role        = Role::User;
        synthetic.text        = m.ui.composer.queued[qi].text;
        synthetic.attachments = m.ui.composer.queued[qi].attachments;
        synthetic.timestamp   = now;
        std::string meta = "queued #" + std::to_string(qi + 1)
                         + " / "     + std::to_string(m.ui.composer.queued.size());
        if (static_cast<int>(qi) == m.ui.composer.queue_peek_idx)
            meta = "\xe2\x9c\x8e editing \xe2\x80\x94 " + meta;   // ✎
        out.push_back(gap_row());
        auto cfg = turn_config(synthetic, base_idx + qi, running_turn, m,
                               /*continuation=*/false,
                               /*meta_override=*/meta);
        out.push_back(maya::Turn{std::move(cfg)}.build());
        ++running_turn;
    }
}

// Locate the live ToolUse a pending_permission is targeting. Walks the
// live tail (runs at/after live_run_start) — the only place a tool can
// still be pre-terminal.
const ToolUse* find_pending_tool(const Model& m) {
    if (!m.d.pending_permission) return nullptr;
    const auto& pp_id = m.d.pending_permission->id;
    const auto& msgs  = m.d.current.messages;
    for (std::size_t i = m.ui.live_run_start; i < msgs.size(); ++i) {
        for (const auto& tc : msgs[i].tool_calls) {
            if (tc.id == pp_id) return &tc;
        }
    }
    return nullptr;
}

// Build the Permission card Element. Floats as its own live_tail row
// below the active assistant Turn (agent_session shape) instead of
// being injected as a Turn body slot — keeps the panel height stable
// when permission appears/disappears. Returns nullopt when the
// pending permission has no matching live ToolUse (corner case during
// run-end races); caller skips the push.
std::optional<maya::Element> build_permission_row(const Model& m) {
    const ToolUse* tc = find_pending_tool(m);
    if (!tc) return std::nullopt;
    return maya::Permission{inline_permission_config(
        *m.d.pending_permission, *tc)}.build();
}

} // namespace

maya::Conversation::Config conversation_config(const Model& m, bool include_frozen) {
    maya::Conversation::Config cfg;

    // ── No borrowed frozen prefix. ───────────────────
    // Under Strata the settled runs are handed to maya as separate
    // sealed NODES (built lazily by build_settled_run), so there is no
    // host-owned Element vector to borrow. cfg.frozen stays null on both
    // paths.
    //   • Strata path (include_frozen=false): render ONLY the live tail
    //     (runs at/after m.ui.live_run_start); the settled runs above are
    //     separate maya nodes.
    //   • Classic/test path (include_frozen=true): render the WHOLE
    //     transcript inline as the live tail (live_start forced to 0), so
    //     view() remains a complete self-contained tree for tests.
    cfg.frozen = nullptr;

    // HUG mode for the Strata LIVE node: when the settled runs are
    // separate sealed nodes (include_frozen=false), this node must hug
    // its live-tail height rather than grow-spacer to the viewport bottom
    // (which would strand a blank void above the composer). Classic
    // monolithic path keeps the fill (true).
    cfg.fill_viewport = include_frozen;

    // The live-tail start: the Strata boundary on the depositional path,
    // 0 (render everything) on the classic path.
    const std::size_t live_start =
        include_frozen ? 0 : std::min(m.ui.live_run_start,
                                      m.d.current.messages.size());

    // ── Live tail. ─────────────────────────────────
    // The only thing rebuilt per frame on the Strata path. Bounded by
    // one in-flight agent turn (one User + possibly several Assistant
    // continuations) plus any queued-message previews. The display turn
    // number seeds from the count of assistant runs settled before the
    // boundary — computed on demand from the messages, not a counter.
    int settled_assistant_runs = 0;
    for (std::size_t k = 0; k < live_start; ) {
        const std::size_t re = turn_run_end(m.d.current.messages, k);
        if (m.d.current.messages[k].role == Role::Assistant)
            ++settled_assistant_runs;
        k = re;
    }
    int running_turn = settled_assistant_runs + 1;
    build_live_tail_from(m, live_start, running_turn, cfg.live_tail,
                         /*allow_continuation=*/!include_frozen);
    build_queued_previews(m, running_turn, cfg.live_tail);

    // Pending permission floats as its own live_tail row below the
    // active assistant Turn (agent_session pattern). Keeps the
    // assistant panel height stable when the prompt appears/disappears,
    // and gives the card the same outer border treatment as in
    // agent_session.
    if (m.d.pending_permission) {
        if (auto e = build_permission_row(m))
            cfg.live_tail.push_back(std::move(*e));
    }

    // Optional shape probe. Set AGENTTY_VIEW_PROF=1 to log every
    // conversation_config invocation's live-tail size plus a rough
    // live-tail message-content sketch. One line per call.
    static const bool view_prof = []{
        const char* e = std::getenv("AGENTTY_VIEW_PROF");
        return e && *e && *e != '0';
    }();
    if (view_prof) {
        static std::FILE* out = std::fopen("/tmp/agentty-view-prof.log", "a");
        if (out) {
            std::size_t live_msgs = (m.d.current.messages.size() > live_start)
                ? (m.d.current.messages.size() - live_start)
                : 0;
            std::size_t live_text_bytes = 0;
            std::size_t live_tool_count = 0;
            for (std::size_t i = live_start;
                 i < m.d.current.messages.size(); ++i) {
                const auto& msg = m.d.current.messages[i];
                live_text_bytes += msg.text.size() + msg.streaming_text.size();
                live_tool_count += msg.tool_calls.size();
            }
            std::fprintf(out,
                "[view] live_tail=%zu live_msgs=%zu "
                "live_text=%zu live_tools=%zu live_start=%zu msgs=%zu\n",
                cfg.live_tail.size(), live_msgs,
                live_text_bytes, live_tool_count, live_start,
                m.d.current.messages.size());
            std::fflush(out);
        }
    }

    // No separate in_flight indicator — the empty-placeholder
    // assistant Turn carries its own "thinking…" body slot during
    // streaming (see build_live_tail), matching agent_session where
    // m.thinking_active produces a body slot inside the assistant
    // Turn rather than a free-floating indicator below it.
    cfg.in_flight = std::nullopt;
    return cfg;
}

} // namespace agentty::ui
