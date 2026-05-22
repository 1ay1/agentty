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
#include <string_view>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/widget/activity_indicator.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/activity_indicator.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

namespace {

// One-row divider used as the seam between every pair of adjacent
// turns — same row frozen.cpp pushes before each settled turn, same
// row build_live_tail pushes between live-tail turns, and same row
// at the frozen↔live boundary. Symmetry across all three sites is
// the invariant: any height delta at a freeze instant would shift
// rows already scrolled into native scrollback against the live
// re-layout, producing a ghost at the scrollback↔viewport seam.
maya::Element gap_row() {
    return maya::Conversation::divider();
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

    bool first_in_tail = true;
    std::size_t i = start;
    while (i < total) {
        const std::size_t run_end = turn_run_end(m.d.current.messages, i);

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
                i, run_end, turn_num, m, /*synthetic=*/false);
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
            const bool tail_is_empty_placeholder =
                tail.role == Role::Assistant
                && tail.text.empty()
                && tail.streaming_text.empty()
                && tail.pending_stream.empty()
                && tail.tool_calls.empty();
            const bool reserve_slot   = m.s.active();
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
            } else if (reserve_slot) {
                // Invisible 2-row spacer that mirrors the
                // ActivityIndicator's natural height (one blank row +
                // one content row, see activity_indicator.hpp's
                // `v(blank(), h(parts))`). Keeps body height stable
                // across the indicator↔content transition.
                using namespace maya::dsl;
                cfg.body.emplace_back(v(text(""), text("")).build());
            }
            out.push_back(maya::Turn{std::move(cfg)}.build());
            ++running_turn;
            i = run_end;
        } else {
            // User (or other non-Assistant) head: single-message Turn.
            auto cfg = turn_config(head, i, turn_num, m,
                                   /*continuation=*/false,
                                   /*synthetic=*/true);
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
                               /*synthetic=*/true,
                               /*meta_override=*/meta);
        out.push_back(maya::Turn{std::move(cfg)}.build());
        ++running_turn;
    }
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
