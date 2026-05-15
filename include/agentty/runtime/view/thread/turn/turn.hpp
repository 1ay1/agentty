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

// Build (or return cached) Element for a single turn. A turn is cached
// once its content is resolved: no in-flight streaming, all tool calls
// terminal, no pending permission targeting it. This is strictly
// broader than the previous "has a successor in messages[]" gate, so
// the just-ended last turn benefits from the cache immediately rather
// than having to wait for the next user message to be appended.
//
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
                                                        bool continuation = false,
                                                        bool synthetic    = false,
                                                        std::string_view meta_override = {});

} // namespace agentty::ui
