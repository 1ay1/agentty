#include "moha/runtime/view/thread/conversation.hpp"

#include <algorithm>
#include <cstddef>

#include "moha/runtime/view/thread/activity_indicator.hpp"
#include "moha/runtime/view/thread/turn/turn.hpp"

namespace moha::ui {

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // Virtualize: older messages live in the terminal's native scrollback
    // (committed via maya::Cmd::commit_scrollback). Preserve absolute
    // turn numbering by reading the running turn count that maybe_virtualize
    // maintains alongside thread_view_start — O(1), regardless of how
    // many turns the session has accumulated.  Previously we walked
    // messages[0..start) here every frame, which was O(thread_view_start)
    // and grew linearly with the conversation; on a long-running session
    // that became the dominant per-frame view cost.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1 + m.ui.thread_view_start_turn;

    cfg.turns.reserve(total - start + m.ui.composer.queued.size());
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        // Continuation: a 2nd+ Assistant in a same-speaker run.  We pass
        // the flag through to turn_config; the Turn widget suppresses its
        // header on continuations and the Conversation widget skips the
        // inter-turn divider, so the per-API-response message structure
        // stays intact (Anthropic's protocol requires it) while three
        // back-to-back agent rounds visually flow as one block under one
        // "Sonnet 4.5" header.
        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        cfg.turns.push_back(turn_config(msg, i, turn, m, continuation));

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
    // msg_idx values are taken from one-past-the-end of the message
    // vector so turn_config's cache check
    // (`msg_idx + 1 < messages.size()`) is structurally false — no
    // entries written for synthetic turns, no stale hits when the queue
    // changes shape between frames.
    if (!m.ui.composer.queued.empty()) {
        auto now = std::chrono::system_clock::now();
        for (std::size_t qi = 0; qi < m.ui.composer.queued.size(); ++qi) {
            Message synthetic;
            synthetic.role      = Role::User;
            synthetic.text      = m.ui.composer.queued[qi];
            synthetic.timestamp = now;
            cfg.turns.push_back(turn_config(synthetic, total + qi, turn,
                                            m, /*continuation=*/false));
            ++turn;
        }
    }

    cfg.in_flight = activity_indicator_config(m);
    return cfg;
}

} // namespace moha::ui
