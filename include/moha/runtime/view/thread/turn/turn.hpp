#pragma once
#include <cstddef>
#include <maya/widget/turn.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Turn config for one message — header + typed body slots
// (PlainText / MarkdownText / AgentTimeline / Permission / cached
// streaming-markdown Element).  Set `continuation = true` when this
// message is the 2nd+ assistant in a same-speaker run; the Turn widget
// then suppresses its header (glyph + label + meta) and the
// Conversation widget skips the inter-turn divider, so consecutive
// assistant messages from one agent action visually flow as one block.
[[nodiscard]] maya::Turn::Config turn_config(const Message& msg,
                                             std::size_t msg_idx,
                                             int turn_num,
                                             const Model& m,
                                             bool continuation = false);

} // namespace moha::ui
