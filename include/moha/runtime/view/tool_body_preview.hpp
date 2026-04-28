#pragma once
#include <maya/widget/tool_body_preview.hpp>
#include "moha/domain/conversation.hpp"

namespace moha::ui {

// ToolUse → ToolBodyPreview::Config. Picks the discriminated body kind
// (CodeBlock / EditDiff / GitDiff / TodoList / Failure / None) based
// on tool name + state and extracts the relevant data.
[[nodiscard]] maya::ToolBodyPreview::Config tool_body_preview_config(
    const ToolUse& tc);

} // namespace moha::ui
