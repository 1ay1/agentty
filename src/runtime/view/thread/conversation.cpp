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
    // turn numbering by counting finalized assistant messages BEFORE the
    // view window too — "turn 42" stays consistent after scrolling back.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1;
    for (std::size_t i = 0; i < start; ++i)
        if (m.d.current.messages[i].role == Role::Assistant) ++turn;

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
