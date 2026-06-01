#pragma once
#include <set>
#include <span>
#include <string>
#include <unordered_map>

#include <maya/widget/tool_body_preview.hpp>
#include "agentty/domain/conversation.hpp"

namespace agentty::ui {

// Cross-tool semantic index built once per turn's tool_calls list:
// path → {line numbers Grep flagged}. A subsequent FileRead on the same
// path inherits these as `highlight_lines`, mirroring agent_session.cpp's
// grep_hits → FileRead wiring so the timeline body anchors the user's
// eye on lines the assistant flagged earlier in the same turn.
using GrepHits = std::unordered_map<std::string, std::set<int>>;

// Walk a tool_calls span, parse each completed Grep tool's markdown-
// structured output (`## Matches in <path>` headers + `### L<start>-<end>`
// blocks), and accumulate (path → start lines) into the returned map.
[[nodiscard]] GrepHits collect_grep_hits(std::span<const ToolUse> tool_calls);

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

// Build-phase flag. When a terminal write/edit/read card is being built
// for the FROZEN snapshot (freeze_range), the full body is rendered with
// show_all=true because that Element is painted exactly ONCE and then
// blitted forever. When it's being built for the LIVE tail (an in-flight
// run whose prior sub-turn carries a big completed card, re-rendered
// every frame until the run settles), the body is elided to a bounded
// window so per-frame cost stays O(window) instead of O(file) — a
// 3000-line read in the live tail measured ~21ms/frame fully expanded,
// ~0.2ms windowed. The frozen snapshot still shows the full body.
//
// freeze_range sets this true for the duration of its build; everywhere
// else (default false) means "live".
class FrozenBuildScope {
public:
    FrozenBuildScope() noexcept;
    ~FrozenBuildScope();
    FrozenBuildScope(const FrozenBuildScope&)            = delete;
    FrozenBuildScope& operator=(const FrozenBuildScope&) = delete;
private:
    bool prev_;
};
[[nodiscard]] bool building_frozen() noexcept;

} // namespace agentty::ui
