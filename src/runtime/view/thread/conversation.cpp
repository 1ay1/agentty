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

#include <chrono>
#include <cstddef>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/widget/activity_indicator.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

namespace {

// Thin dim ─ rule — same as the one frozen.cpp interleaves between
// settled turns, used between live-tail turns.
maya::Element gap_row() {
    return maya::Conversation::divider();
}

// Leading separator between the frozen prefix and the first live
// turn. A bare divider() gets visually eaten on the seam: the
// width-aware component can resolve to blank() on the first frame
// (w==0 during initial layout / resize), and even when it draws,
// the adjacent Turn's top chrome sits flush against it. Sandwich
// it with explicit blank rows so the seam is always visible.
maya::Element leading_seam_row() {
    using namespace maya::dsl;
    return v(blank(), maya::Conversation::divider(), blank()).build();
}

// Sentinel-check: assistant message whose only content is tool_calls
// (no prose). Mirrors freeze_range's predicate so live and frozen
// apply the same batch-merge policy.
bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Build the live-tail Elements. Mirrors freeze_range's shape so the
// live visual matches what eventually lands in m.ui.frozen:
//   • One leading separator before each fresh-speaker turn (skipped
//     for the very first row of a fresh thread).
//   • A run of consecutive tool-only Assistant continuations
//     collapses into ONE merged Turn whose tool_calls are the union
//     of the run's. Without this the live tail stacks N ACTIONS
//     panels back-to-back with no separator (continuation suppresses
//     the gap), which is the bug the screenshot captures.
std::vector<maya::Element> build_live_tail(const Model& m, int& running_turn) {
    std::vector<maya::Element> rows;
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = std::min(m.ui.frozen_through, total);
    if (start >= total) return rows;

    rows.reserve((total - start) * 2);

    bool first_in_tail = true;
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];

        const bool empty_placeholder =
            msg.role == Role::Assistant
            && msg.text.empty()
            && msg.streaming_text.empty()
            && msg.tool_calls.empty();

        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        const bool need_gap = !continuation
            && !(first_in_tail && m.ui.frozen.empty() && i == 0);
        if (need_gap) {
            // First seam off the frozen prefix gets the padded variant
            // so the rule isn't crushed against the previous Turn's
            // bottom chrome or lost to a w==0 first frame. Subsequent
            // intra-tail seams use the plain one-row divider.
            rows.push_back(first_in_tail && !m.ui.frozen.empty()
                               ? leading_seam_row()
                               : gap_row());
        }
        first_in_tail = false;

        // Tool-batch merge: collapse a run of tool-only Assistant
        // continuations into the head message's panel. Identical
        // policy to freeze_range — keeps the visual stable across
        // the live→frozen transition.
        std::size_t run_end = i + 1;
        while (run_end < total
               && m.d.current.messages[run_end].role == Role::Assistant
               && is_tool_only_assistant(m.d.current.messages[run_end])) {
            ++run_end;
        }

        int turn_num = running_turn;

        if (run_end > i + 1) {
            Message merged = msg;
            std::size_t reserve_n = msg.tool_calls.size();
            for (std::size_t j = i + 1; j < run_end; ++j)
                reserve_n += m.d.current.messages[j].tool_calls.size();
            merged.tool_calls.reserve(reserve_n);
            for (std::size_t j = i + 1; j < run_end; ++j) {
                const auto& src = m.d.current.messages[j].tool_calls;
                merged.tool_calls.insert(merged.tool_calls.end(),
                                         src.begin(), src.end());
            }
            auto cfg = turn_config(merged, i, turn_num, m, continuation,
                                   /*synthetic=*/true);
            rows.push_back(maya::Turn{std::move(cfg)}.build());
            if (msg.role == Role::Assistant && !continuation) ++running_turn;
            i = run_end - 1;   // for-loop ++ lands on run_end
            continue;
        }

        auto cfg = turn_config(msg, i, turn_num, m, continuation,
                               /*synthetic=*/true);
        // Empty placeholder: inject an animated activity indicator so
        // the Turn has visible, MOVING content from submit through
        // first delta. Uses the same m.s.spinner the bottom-bar
        // indicator advances on each tick (see meta.cpp), so the
        // glyph cycles every frame instead of sitting as a static
        // "… thinking" string.
        if (empty_placeholder) {
            using namespace maya::dsl;
            maya::ActivityIndicator::Config ind;
            ind.edge_color    = cfg.rail_color;
            ind.spinner_glyph = std::string{m.s.spinner.current_frame()};
            ind.label         = "thinking";
            cfg.body.emplace_back(
                maya::ActivityIndicator{std::move(ind)}.build());
        }
        rows.push_back(maya::Turn{std::move(cfg)}.build());
        if (msg.role == Role::Assistant && !continuation) ++running_turn;
    }
    return rows;
}

// Build the queued-message preview rows: visible at the tail of the
// transcript so the user can see what's queued. Mirrors Claude
// Code's appearance at offset 80106500 — visually identical to real
// user turns; the "queued not sent" cue is absence-of-assistant +
// the composer's `❚ N queued` chip.
std::vector<maya::Element> build_queued_previews(const Model& m, int& running_turn) {
    std::vector<maya::Element> rows;
    if (m.ui.composer.queued.empty()) return rows;
    rows.reserve(m.ui.composer.queued.size() * 2);
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
        rows.push_back(gap_row());
        auto cfg = turn_config(synthetic, base_idx + qi, running_turn, m,
                               /*continuation=*/false,
                               /*synthetic=*/true,
                               /*meta_override=*/meta);
        rows.push_back(maya::Turn{std::move(cfg)}.build());
        ++running_turn;
    }
    return rows;
}

} // namespace

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // ── Borrowed frozen prefix (zero-copy). ─────────────────────────
    // maya renders this through list_ref, so growing m.ui.frozen does
    // not increase per-frame cost. Maya's hash_id-keyed cell cache
    // makes already-painted Elements hit on every subsequent frame.
    cfg.frozen = &m.ui.frozen;

    // ── Live tail. ──────────────────────────────────────────────────
    // The only thing rebuilt per frame. Bounded by one in-flight
    // agent turn (one User + possibly several Assistant continuations)
    // plus any queued-message previews.
    int running_turn = m.ui.frozen_turn + 1;
    auto live = build_live_tail(m, running_turn);
    auto queued = build_queued_previews(m, running_turn);
    cfg.live_tail.reserve(live.size() + queued.size());
    for (auto& e : live)   cfg.live_tail.push_back(std::move(e));
    for (auto& e : queued) cfg.live_tail.push_back(std::move(e));

    // No separate in_flight indicator — the empty-placeholder
    // assistant Turn carries its own "thinking…" body slot during
    // streaming (see build_live_tail), matching agent_session where
    // m.thinking_active produces a body slot inside the assistant
    // Turn rather than a free-floating indicator below it.
    cfg.in_flight = std::nullopt;
    return cfg;
}

} // namespace agentty::ui
