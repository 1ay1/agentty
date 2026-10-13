// tool_body_preview — `task` (subagent) tool body.
//
// Live activity feed while running, condensed report when settled. The
// feed is streamed into progress_text() via progress::emit (turns / tool
// calls / results / streamed text); the terminal output is the harvested
// "Subagent report". BashOutput gives the tail-oriented "watch it work"
// look while running; CodeBlock shows the full report once settled.

#include "tool_body_common.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

bool task_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;

    // Card body ceilings: the LIVE feed gets more room (it's the only
    // window into what the subagent is doing — ⚙ calls, ✓/✗ results,
    // ↻ retries — and it's transient), while the SETTLED report stays a
    // tight glance-able summary; the full report is in the transcript
    // and the Ctrl+O viewer.
    constexpr std::size_t kTaskLiveRows   = 8;
    constexpr std::size_t kTaskReportRows = 5;
    if (tc.is_running()) {
        if (!tc.progress_text().empty()) {
            out.kind = Kind::BashOutput;
            // BashOutput is tail-oriented (newest activity at the
            // bottom) — keep the last few feed lines so the running
            // card shows the CURRENT step, not the stale header.
            out.text = tail_window(tc.progress_text(), kTaskLiveRows);
            out.text_color = text_tertiary;
            out.is_streaming = true;
        }
        return true;
    }
    if (tc.is_terminal() && !tc.output().empty()) {
        // Strip the redundant "Subagent report (type, N turns):"
        // header + the blank line after it — the card detail already
        // shows the type and turn count, so the body should lead with
        // the actual outcome line.
        std::string_view body = tc.output();
        if (auto nl = body.find('\n'); nl != std::string_view::npos
            && body.substr(0, nl).find("Subagent report") != std::string_view::npos) {
            body.remove_prefix(nl + 1);
            while (!body.empty() && (body.front() == '\n' || body.front() == ' '))
                body.remove_prefix(1);
        }
        out.kind = Kind::CodeBlock;
        // keep content lines + 1 "⋯ N more" marker = ceiling rows max.
        out.text = head_window(body, kTaskReportRows - 1);
        out.text_color = text_tertiary;
        // We pre-sliced to the ceiling; render it verbatim (no further
        // head+tail elision that would fight our marker).
        out.show_all = true;
    }
    return true;
}

} // namespace agentty::ui::detail
