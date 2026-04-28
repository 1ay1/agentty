#pragma once
#include <maya/widget/conversation.hpp>
#include "moha/runtime/model.hpp"

namespace moha::ui {

// Build the Conversation config — list of Turn::Configs (one per
// visible message in the view window) plus an optional bottom
// in-flight indicator. Called from thread_config() when the thread
// is non-empty.
[[nodiscard]] maya::Conversation::Config conversation_config(const Model& m);

} // namespace moha::ui
