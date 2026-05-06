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

    cfg.turns.reserve(total - start);
    for (std::size_t i = start; i < total; ++i) {
        const auto& msg = m.d.current.messages[i];
        cfg.turns.push_back(turn_config(msg, i, turn, m));
        if (msg.role == Role::Assistant) ++turn;
    }

    cfg.in_flight = activity_indicator_config(m);
    return cfg;
}

} // namespace moha::ui
