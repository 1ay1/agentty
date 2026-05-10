#pragma once
#include <maya/widget/thread.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the Thread widget config from Model. Pure data extraction —
// the widget owns all rendering chrome (welcome screen, conversation
// layout, in-flight indicator, per-turn rail).
[[nodiscard]] maya::Thread::Config thread_config(const Model& m);

} // namespace agentty::ui
