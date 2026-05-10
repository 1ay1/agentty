#pragma once
#include <maya/widget/agent_timeline.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the assistant turn's "Actions" panel config. Aggregates state
// (total/done/elapsed/category counts), picks per-category colors,
// computes title/footer, walks tool_calls into events.
[[nodiscard]] maya::AgentTimeline::Config agent_timeline_config(
    const Message& msg, int spinner_frame, maya::Color rail_color);

} // namespace agentty::ui
