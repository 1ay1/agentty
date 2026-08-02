// tool_body_preview_config — the ToolUse → ToolBodyPreview::Config
// adapter. Structured tool names dispatch to purpose-built renderers; one
// lifecycle fallback then guarantees a useful body for every state, including
// native tools without a custom renderer and dynamically discovered MCP tools.
// Shared parsing/windowing helpers live in tool_body_common.{hpp,cpp}.
//
// Dispatch order remains load-bearing: failed todo calls bypass the checkbox
// renderer so their error is shown, while edit/write/bash/task retain their
// specialized output whenever it is meaningful.

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "tool_body_common.hpp"

#include <cctype>

#include "agentty/tool/util/utf8.hpp"

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

bool text_is_visible(std::string_view text) {
    const auto stripped = tools::util::strip_terminal_controls(text);
    for (unsigned char c : stripped) {
        if (!std::isspace(c) && c >= 0x20) return true;
    }
    return false;
}

bool has_visible_body(const maya::ToolBodyPreview::Config& body) {
    using Kind = maya::ToolBodyPreview::Kind;
    switch (body.kind) {
    case Kind::None:      return false;
    case Kind::EditDiff:  return !body.hunks.empty();
    case Kind::TodoList:  return !body.todos.empty();
    // Whitespace is meaningful in file bodies because their gutter/prefix and
    // footer make it visible (an empty-line-only file is still real output).
    case Kind::FileRead:
    case Kind::FileWrite: return !body.text.empty();
    default:              return text_is_visible(body.text);
    }
}

// No tool card should ever be an unexplained blank. Structured renderers get
// first refusal; this lifecycle fallback covers every native tool as well as
// dynamically discovered MCP tools, including tools added after this code was
// compiled. Live progress wins, terminal output is preserved, and genuinely
// silent states receive a short explicit explanation.
void generic_lifecycle_body(const ToolUse& tc,
                            maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (has_visible_body(out)) return;

    out.text_color = text_tertiary;
    if (tc.is_failed()) {
        out.kind = Kind::Failure;
        out.text = text_is_visible(tc.output())
            ? std::string{tc.output()}
            : "Tool failed without an error message.";
        out.chrome_color = status_error;
        return;
    }
    if (tc.is_running()) {
        out.kind = Kind::CodeBlock;
        // Keep generic output on the same three-row CodeBlock geometry used
        // after settle. Dedicated subprocess/task renderers still use their
        // richer BashOutput view; unknown and future tools remain seam-safe.
        if (text_is_visible(tc.progress_text())) {
            out.text = tc.progress_text();
            out.is_streaming = true;
        } else {
            out.text = "Waiting for tool output\xe2\x80\xa6";
        }
        return;
    }
    if (tc.is_pending()) {
        out.kind = Kind::CodeBlock;
        out.text = "Preparing tool input\xe2\x80\xa6";
        return;
    }
    if (tc.is_approved()) {
        out.kind = Kind::CodeBlock;
        out.text = "Permission approved \xe2\x80\x94 starting\xe2\x80\xa6";
        return;
    }
    if (tc.is_done()) {
        out.kind = Kind::CodeBlock;
        out.text = text_is_visible(tc.output())
            ? std::string{tc.output()}
            : "Completed successfully \xe2\x80\x94 no output.";
        return;
    }

    // Rejected is the sole remaining ToolUse state. Keep it neutral rather
    // than painting it as a runtime failure: the status icon already conveys
    // that the call was declined.
    out.kind = Kind::CodeBlock;
    out.text = "Tool was not run.";
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
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // Chrome (line-number gutter, pipe separator, elision marker) reads
    // in the tool's category color so the body's structure visually
    // matches the header NAME color. Body content itself stays
    // text_tertiary (dim) so the colored chrome frames it without
    // competing with prose. Each per-tool renderer may override
    // chrome_color (e.g. the Failure paths flip it to red).
    out.chrome_color = tool_category_color(n);

    // ── Structured renderers get first refusal. Every return passes through
    //    the generic lifecycle fallback so even silent/empty tools explain
    //    what they are waiting for or how they settled. ───────────────────
    if (n == "edit") {
        detail::edit_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "bash" || n == "diagnostics" || n == "test") {
        detail::bash_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "write") {
        detail::write_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }

    if (n == "git_diff") {
        if (detail::git_diff_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "read" || n == "find_definition") {
        if (detail::read_body(tc, grep_hits, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "web_fetch") {
        if (detail::web_fetch_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }
    if (n == "grep" || n == "glob" || n == "list_dir"
        || n == "web_search"
        || n == "git_status" || n == "git_log" || n == "git_commit") {
        if (detail::generic_list_body(tc, out)) {
            generic_lifecycle_body(tc, out);
            return out;
        }
    }

    if (n == "task") {
        detail::task_body(tc, out);
        generic_lifecycle_body(tc, out);
        return out;
    }

    // Preserve the deliberate ordering: a failed todo displays the error,
    // not its stale checkbox list. All other todo states keep the structured
    // list when one is available.
    if (tc.is_failed()) {
        generic_lifecycle_body(tc, out);
        return out;
    }
    if (n == "todo" && detail::todo_body(tc, out)) {
        generic_lifecycle_body(tc, out);
        return out;
    }

    generic_lifecycle_body(tc, out);
    return out;
}

} // namespace agentty::ui
