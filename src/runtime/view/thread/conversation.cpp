// conversation.cpp — view adapter for the conversation viewport.
//
// agent_session-style fast path: hand maya a borrowed pointer to
// m.ui.frozen (the append-only built-Element vector that grows on
// every settled turn) plus a small live-tail of unfrozen Elements
// (the in-flight assistant turn + any queued-message previews).
//
// The per-frame cost is therefore O(visible_live_tail) regardless of
// how long the session has run. Settled turns are NEVER rebuilt:
// they were built into Element values inside m.ui.frozen at the
// moment they settled (see src/runtime/app/update/frozen.cpp) and
// stay there until thread switch / NewThread / compaction triggers
// a rebuild.

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
void build_live_tail(const Model& m, int& running_turn,
                     std::vector<maya::Element>& out) {
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = std::min(m.ui.frozen_through, total);
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
        if (compaction_boundary_at(i)) {
            out.push_back(compaction_divider_row());
        }

        const bool first_overall = m.ui.frozen.empty() && first_in_tail && i == 0;
        if (!first_overall) {
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
                i, run_end, turn_num, m);
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

            // Cache the settled-but-not-yet-frozen run. A run sitting in
            // the live tail with every tool terminal and no active
            // stream is byte-stable: its body (which may include a
            // multi-thousand-line write/edit card) won't change until
            // freeze_through moves it into m.ui.frozen. Without a
            // hash_id the live tail has no cache entry, so maya REBUILDS
            // and REPAINTS that whole body every frame — a 3000-line
            // write in the tail measured ~80ms/frame (o1_probe
            // livetail_ms column). Stamping the SAME key freeze_range
            // will use means: (a) the body paints once and blits
            // thereafter while it waits to freeze, and (b) the cache
            // entry survives the freeze handoff (identical key) so the
            // freeze instant is seamless. Only when the run is fully
            // terminal AND no indicator/spinner slot was added (those
            // mutate per frame and must miss).
            const bool run_terminal = [&] {
                for (std::size_t j = i; j < run_end; ++j)
                    for (const auto& tc : m.d.current.messages[j].tool_calls)
                        if (!tc.is_terminal()) return false;
                return true;
            }();
            // CRITICAL: do NOT stamp the cacheable key while the reveal
            // overlay is still animating. assistant_run_hash_id is keyed on
            // the message id + compute_render_key() (text content/size) — it
            // is INVARIANT across the reveal's scramble→clean transition
            // because the underlying text bytes don't change, only the
            // per-frame overlay cells do. If we stamp it while the trailing
            // edge is still showing scramble glyphs, maya paints those
            // scramble cells into its hash_id-keyed component cache; when the
            // reveal then settles to clean text the key is UNCHANGED, so the
            // cache HITS and never repaints — stranding scramble garbage on
            // the settled tail forever (the frozen-glyph screenshot:
            // "just let me\u2423Z@o%"). And because freeze_range reuses the same
            // key, the garbage is frozen permanently. Leave the run UNKEYED
            // (rebuild + repaint every frame, like the in-flight run) until
            // every message's reveal widget has fully drained — then the
            // clean cells are what gets cached, and the freeze handoff hits a
            // clean entry. The drain window is ~200 ms ramp + ~376 ms
            // scramble settle, fully covered by the pending_settle_freeze
            // RAF clock, so this costs at most a few extra rebuilds.
            const bool reveal_settled = [&] {
                for (std::size_t j = i; j < run_end && j < m.d.current.messages.size(); ++j) {
                    const auto& mj = m.d.current.messages[j];
                    if (mj.role != Role::Assistant || mj.text.empty()) continue;
                    const auto& mc = m.ui.view_cache.message_md(
                        m.d.current.id, mj.id);
                    if (!mc.streaming) continue;
                    if (mc.streaming->is_live()
                     || mc.streaming->is_finalizing()
                     || mc.streaming->reveal_in_progress()
                     || mc.streaming->is_parsing())
                        return false;
                }
                return true;
            }();
            if (run_terminal && !reserve_slot && reveal_settled) {
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
            auto cfg = turn_config(head, i, turn_num, m,
                                   /*continuation=*/false);
            out.push_back(maya::Turn{std::move(cfg)}.build());
            // User turns do not bump running_turn — the running count
            // is over Assistant turns (matches frozen.cpp's policy).
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

// Locate the live ToolUse a pending_permission is targeting. Walks
// the unfrozen tail (the only place a tool can still be pre-terminal).
const ToolUse* find_pending_tool(const Model& m) {
    if (!m.d.pending_permission) return nullptr;
    const auto& pp_id = m.d.pending_permission->id;
    const auto& msgs  = m.d.current.messages;
    for (std::size_t i = m.ui.frozen_through; i < msgs.size(); ++i) {
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

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // ── Borrowed frozen prefix (zero-copy). ─────────────
    // maya renders this through list_ref, so growing m.ui.frozen does
    // not increase per-frame cost. Maya's hash_id-keyed cell cache
    // makes already-painted Elements hit on every subsequent frame.
    cfg.frozen = &m.ui.frozen;

    // ── Live tail. ─────────────────────────────────
    // The only thing rebuilt per frame. Bounded by one in-flight
    // agent turn (one User + possibly several Assistant continuations)
    // plus any queued-message previews.
    int running_turn = m.ui.frozen_turn + 1;
    build_live_tail(m, running_turn, cfg.live_tail);
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
    // conversation_config invocation's frozen/live_tail sizes plus
    // a rough live-tail message-content sketch. One line per call.
    static const bool view_prof = []{
        const char* e = std::getenv("AGENTTY_VIEW_PROF");
        return e && *e && *e != '0';
    }();
    if (view_prof) {
        static std::FILE* out = std::fopen("/tmp/agentty-view-prof.log", "a");
        if (out) {
            std::size_t live_msgs = (m.d.current.messages.size()
                > m.ui.frozen_through)
                ? (m.d.current.messages.size() - m.ui.frozen_through)
                : 0;
            std::size_t live_text_bytes = 0;
            std::size_t live_tool_count = 0;
            for (std::size_t i = m.ui.frozen_through;
                 i < m.d.current.messages.size(); ++i) {
                const auto& msg = m.d.current.messages[i];
                live_text_bytes += msg.text.size() + msg.streaming_text.size();
                live_tool_count += msg.tool_calls.size();
            }
            std::fprintf(out,
                "[view] frozen=%zu live_tail=%zu live_msgs=%zu "
                "live_text=%zu live_tools=%zu frozen_through=%zu msgs=%zu\n",
                m.ui.frozen.size(), cfg.live_tail.size(), live_msgs,
                live_text_bytes, live_tool_count, m.ui.frozen_through,
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
