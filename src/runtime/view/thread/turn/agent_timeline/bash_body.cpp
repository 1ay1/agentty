// tool_body_preview — `bash` / `diagnostics` tool body.
//
// BashOutput (tail-oriented, structured-extraction fallback chain in maya
// picks up gtest-style `N tests passed` summaries and compiler-error rows,
// otherwise falls back to a dim 4-row tail). `failed` is wired for the
// inline `· exit N` suffix on the last line of failed output. We keep
// BashOutput on failure (instead of routing to Kind::Failure) so the
// timeline card border + status icon carry the failure signal and the body
// stays calm — agent_session.cpp's "no double-flagging" discipline.

#include "tool_body_common.hpp"

#include <string>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

bool bash_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;

    if (tc.is_running() && !tc.progress_text().empty()) {
        out.kind = Kind::BashOutput;
        out.text = tc.progress_text();
        out.text_color = text_tertiary;
        out.is_streaming = true;
        return true;
    }
    if (tc.is_terminal()) {
        auto stripped = strip_bash_output_fence(tc.output());
        if (!stripped.empty()) {
            out.kind = Kind::BashOutput;
            out.text = std::move(stripped);
            out.text_color = text_tertiary;
            out.failed = tc.is_failed();
        }
        return true;
    }
    return true;
}

} // namespace agentty::ui::detail
