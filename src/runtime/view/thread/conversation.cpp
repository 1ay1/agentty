#include "agentty/runtime/view/thread/conversation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>

#include "agentty/runtime/view/thread/activity_indicator.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // Virtualize: older messages live in the terminal's native scrollback
    // (committed via maya::Cmd::commit_scrollback). Preserve absolute
    // turn numbering by reading the running turn count that maybe_virtualize
    // maintains alongside thread_view_start — O(1), regardless of how
    // many turns the session has accumulated.
    //
    // During compaction the messages vector ends with two synthetic
    // entries: the "summarise per spec" User prompt and an Assistant
    // placeholder receiving the streamed summary. Both are
    // bookkeeping the user shouldn't see — rendering them visibly
    // would (a) show the long synthetic prompt as a fake user turn,
    // (b) stream the model's summary in real time as if the
    // assistant were answering a normal question, and then (c) jump
    // to a much shorter post-compact conversation when the swap
    // happens. The status banner already conveys "compacting
    // context…"; clipping `total` to the pre-synth count keeps the
    // visible conversation in its pre-compact shape until the swap
    // lands, so the user only ever sees the boundary divider appear,
    // not the compaction process itself.
    std::size_t total = m.d.current.messages.size();
    if (m.s.compacting
        && m.s.compact_pre_synth_count > 0
        && m.s.compact_pre_synth_count <= total) {
        total = m.s.compact_pre_synth_count;
    }
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1 + m.ui.thread_view_start_turn;

    // Use the agent_session-style fast path: emit pre-built turn
    // Elements via maya::Conversation's `built_turns` field. Settled
    // turns are served from the (thread, msg_idx) → Element cache and
    // skip Turn::build() entirely on every frame after the first;
    // only the live tail rebuilds. Without this, maya::Conversation::
    // build() called `Turn{cfg}.build()` per visible turn per frame,
    // which laid out every tool card, every markdown block, and every
    // permission row from scratch — the dominant per-frame cost on a
    // long session. Mirrors what the agent_session example achieves
    // with `m.frozen` + `list_ref(...)`: build once per turn lifetime,
    // render-by-reference forever after.
    cfg.built_turns.reserve(total - start + m.ui.composer.queued.size());
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        // Continuation: a 2nd+ Assistant in a same-speaker run. The Turn
        // widget suppresses its header on continuations and the
        // Conversation widget skips the inter-turn divider, so the
        // per-API-response message structure stays intact (Anthropic's
        // protocol requires it) while three back-to-back agent rounds
        // visually flow as one block under one "Sonnet 4.5" header.
        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        cfg.built_turns.push_back(turn_element(msg, i, turn, m, continuation));

        // Increment the user-visible turn number only on the FIRST
        // assistant of a run.  Continuations share the run's number so
        // the next user message gets the next sequential turn (otherwise
        // a 5-round agent action would push the next user to turn N+5).
        if (msg.role == Role::Assistant && !continuation) ++turn;
    }

    // Queued-message previews: render typed-but-not-yet-sent messages as
    // user turns at the tail of the transcript so the user can SEE what
    // they queued, not just a count. Mirrors Claude Code's behaviour
    // (offset 80106500 in the 2.1.119 binary): no special glyph, no
    // "queued:" label, no dim modifier — visually identical to a real
    // user message. The "this is queued, not sent yet" cue comes from
    // (a) the absence of a following assistant turn and (b) the
    // composer's `❚ N queued` chip.
    //
    // `synthetic = true` opts these per-frame Message instances out of
    // the Element cache. Each call default-constructs the Message with
    // a fresh MessageId, so caching them would only fill the LRU with
    // garbage entries.
    if (!m.ui.composer.queued.empty()) {
        auto now = std::chrono::system_clock::now();
        for (std::size_t qi = 0; qi < m.ui.composer.queued.size(); ++qi) {
            Message synthetic;
            synthetic.role      = Role::User;
            synthetic.text      = m.ui.composer.queued[qi];
            synthetic.timestamp = now;
            cfg.built_turns.push_back(turn_element(synthetic, total + qi,
                                                   turn, m, /*continuation=*/false,
                                                   /*synthetic=*/true));
            ++turn;
        }
    }

    cfg.in_flight = activity_indicator_config(m);
    return cfg;
}

} // namespace agentty::ui
