#pragma once
#include <cstddef>
#include <string_view>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the Turn config for one message — header + typed body slots
// (PlainText / MarkdownText / AgentTimeline / Permission / cached
// streaming-markdown Element).  Set `continuation = true` when this
// message is the 2nd+ assistant in a same-speaker run; the Turn widget
// then suppresses its header (glyph + label + meta) and the
// Conversation widget skips the inter-turn divider, so consecutive
// assistant messages from one agent action visually flow as one block.
//
// `synthetic = true` marks the per-frame queued-message preview turns
// the conversation adapter manufactures from `composer.queued[]`.
// Those messages carry a fresh `MessageId` each frame, so caching
// them would only fill the LRU with garbage entries; the flag short-
// circuits the cache write path.
[[nodiscard]] maya::Turn::Config turn_config(const Message& msg,
                                             std::size_t msg_idx,
                                             int turn_num,
                                             const Model& m,
                                             bool continuation = false,
                                             bool synthetic    = false,
                                             std::string_view meta_override = {});

} // namespace agentty::ui
