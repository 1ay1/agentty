// tool_body_preview_config — the ToolUse → ToolBodyPreview::Config
// adapter. This file is now just the DISPATCHER: it picks the discriminated
// body Kind by tool NAME + state and delegates to one `<tool>_body(...)`
// renderer per tool, each living in its own translation unit
// (edit_body.cpp, bash_body.cpp, write_body.cpp, …). The shared parsing /
// windowing helpers live in tool_body_common.{hpp,cpp}.
//
// Dispatch ORDER is load-bearing and mirrors the original monolithic
// control flow exactly: the shared Failure fallback sits BETWEEN task and
// todo, so a failed `todo` renders as a plain error body (it never reaches
// the todo checkbox path), while task consumes its own failure case.

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "tool_body_common.hpp"

namespace agentty::ui {

// ── Frozen-build scope ─────────────────────────────────────
// Retained as a no-op-by-default phase flag for callers that still ask
// `building_frozen()`. The body is now IDENTICAL in both phases (full,
// seam-safe), so no renderer path branches on it for content; freeze_range
// still scopes it for clarity and so a future divergent-build path has a
// hook.
namespace {
bool& frozen_build_flag() noexcept {
    thread_local bool v = false;
    return v;
}
} // namespace

FrozenBuildScope::FrozenBuildScope() noexcept
    : prev_(frozen_build_flag()) { frozen_build_flag() = true; }
FrozenBuildScope::~FrozenBuildScope() { frozen_build_flag() = prev_; }
bool building_frozen() noexcept { return frozen_build_flag(); }

GrepHits collect_grep_hits(std::span<const ToolUse> tool_calls) {
    GrepHits out;
    for (const auto& tc : tool_calls) {
        if (tc.name.value != "grep") continue;
        const auto& body = tc.output();
        if (body.empty()) continue;
        detail::accumulate_grep_hits(body, out);
    }
    return out;
}

maya::ToolBodyPreview::Config tool_body_preview_config(
    const ToolUse& tc, const GrepHits* grep_hits)
{
    using Kind = maya::ToolBodyPreview::Kind;
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // Chrome (line-number gutter, pipe separator, elision marker) reads
    // in the tool's category color so the body's structure visually
    // matches the header NAME color. Body content itself stays
    // text_tertiary (dim) so the colored chrome frames it without
    // competing with prose. Each per-tool renderer may override
    // chrome_color (e.g. the Failure paths flip it to red).
    out.chrome_color = tool_category_color(n);

    // ── Tools that fully OWN their name — always consume it (return the
    //    Config they built, even if the body ends up empty). ────────────
    if (n == "edit")  { detail::edit_body(tc, out);  return out; }
    if (n == "bash" || n == "diagnostics") { detail::bash_body(tc, out); return out; }
    if (n == "write") { detail::write_body(tc, out); return out; }

    // ── Tools whose renderer returns false to FALL THROUGH to the shared
    //    Failure fallback below (non-terminal / failed states). ──────────
    if (n == "git_diff") {
        if (detail::git_diff_body(tc, out)) return out;
    }
    if (n == "read" || n == "find_definition") {
        if (detail::read_body(tc, grep_hits, out)) return out;
        // failed read → fall through to Failure fallback (a red error body
        // reads more naturally than a FileRead gutter over an error).
    }
    if (n == "web_fetch") {
        if (detail::web_fetch_body(tc, out)) return out;
    }
    if (n == "grep" || n == "glob" || n == "list_dir"
        || n == "web_search"
        || n == "git_status" || n == "git_log" || n == "git_commit") {
        if (detail::generic_list_body(tc, out)) return out;
    }

    // ── task (subagent): live feed / condensed report. Owns its name. ───
    if (n == "task") { detail::task_body(tc, out); return out; }

    // ── Failure fallback. Chrome flips to red so the body chrome matches
    //    the card's failure cue; body content stays dim. NOTE: this sits
    //    ABOVE todo on purpose — a failed todo renders here, not as a
    //    checkbox list.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = std::string{tc.output()};
        out.chrome_color = status_error;
        return out;
    }

    // ── Todo: structured checkbox list. ─────────────────────────────────
    if (n == "todo") {
        if (detail::todo_body(tc, out)) return out;
    }

    return out;     // kind = None
}

} // namespace agentty::ui
