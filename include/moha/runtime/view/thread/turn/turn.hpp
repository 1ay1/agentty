#pragma once
#include <cstddef>
#include <maya/widget/conversation.hpp>
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

// Build (or return cached) Element for a single turn. Settled turns —
// those whose `msg_idx + 1 < messages.size()` — go through the
// (thread, msg_idx) → Element cache; live turns rebuild every frame.
// Use this in preference to `turn_config()` + manual `Turn{cfg}.build()`
// at the conversation layer: the cached Element skips the entire
// per-frame Turn::build() reconstruction (agent_timeline + every tool
// card + markdown body + permission rows), which is the dominant cost
// on a long session. Mirrors the agent_session example's m.frozen
// pattern: build once per turn lifetime, render-by-reference after.
[[nodiscard]] maya::Conversation::PreBuilt turn_element(const Message& msg,
                                                        std::size_t msg_idx,
                                                        int turn_num,
                                                        const Model& m,
                                                        bool continuation = false);

} // namespace moha::ui
