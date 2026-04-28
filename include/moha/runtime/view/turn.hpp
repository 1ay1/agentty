#pragma once
#include <cstddef>
#include <maya/widget/turn.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Turn config for one message — header + typed body slots
// (PlainText / MarkdownText / AgentTimeline / Permission / cached
// streaming-markdown Element).
[[nodiscard]] maya::Turn::Config turn_config(const Message& msg,
                                             std::size_t msg_idx,
                                             int turn_num,
                                             const Model& m);

} // namespace moha::ui
