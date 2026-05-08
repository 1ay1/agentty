#pragma once
#include <set>
#include <string>
#include <unordered_map>

#include <maya/widget/tool_body_preview.hpp>
#include "moha/domain/conversation.hpp"

namespace moha::ui {

// Cross-tool semantic index built once per Message's tool_calls list:
// path → {line numbers Grep flagged}. A subsequent FileRead on the same
// path inherits these as `highlight_lines`, mirroring agent_session.cpp's
// grep_hits → FileRead wiring so the timeline body anchors the user's
// eye on lines the assistant flagged earlier in the same turn.
using GrepHits = std::unordered_map<std::string, std::set<int>>;

// Walk `msg.tool_calls`, parse each completed Grep tool's markdown-
// structured output (`## Matches in <path>` headers + `### L<start>-<end>`
// blocks), and accumulate (path → start lines) into the returned map.
[[nodiscard]] GrepHits collect_grep_hits(const Message& msg);

// ToolUse → ToolBodyPreview::Config. Picks the discriminated body kind
// (BashOutput / FileRead / FileWrite / Json / EditDiff / GitDiff /
// GrepMatches / TodoList / CodeBlock / Failure / None) based on tool
// name + state and extracts the relevant data.
//
// `grep_hits` (when provided) lets a `read` tool pick up line numbers
// flagged by an earlier Grep on the same path, surfacing them as
// `highlight_lines` in the rendered FileRead body.
[[nodiscard]] maya::ToolBodyPreview::Config tool_body_preview_config(
    const ToolUse& tc, const GrepHits* grep_hits = nullptr);

} // namespace moha::ui
