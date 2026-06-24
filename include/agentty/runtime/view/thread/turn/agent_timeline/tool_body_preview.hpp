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

// Build-phase flag. Set true (via FrozenBuildScope) while freeze_range
// builds the frozen snapshot; false (default) while the live tail is
// built each frame.
//
// HISTORY: this used to switch the tool-card body between a FULL render
// (frozen) and a bounded head+tail WINDOW (live) to keep per-frame cost
// O(window). That was REMOVED — windowing the live card made it commit
// DIFFERENT rows to native scrollback than the full frozen card, shifting
// the committed prefix at the freeze handoff (the duplicated/wiped-card
// scrollback corruption). The body is now the FULL content in BOTH phases,
// byte-identical, so the handoff is a pure cache hit. Per-frame cost stays
// bounded a different way: every terminal tool card carries a content-
// addressed per-event hash_id (agent_timeline.cpp), so maya captures its
// painted cells once and BLITS them every subsequent frame — a tall
// settled card in the live tail is paint-once, not re-laid-out per frame.
//
// The flag is retained (still scoped by freeze_range) as a no-op-by-
// default hook: nothing branches on it for body CONTENT anymore, but it
// gives a future divergent-build path a place to key off without
// reintroducing a live/frozen body mismatch.
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
