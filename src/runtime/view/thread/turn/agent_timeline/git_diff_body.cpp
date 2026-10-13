// tool_body_preview — `git_diff` tool body.
//
// GitDiff: per-line +/-/@@ coloring (GitDiff owns the palette). Only a
// DONE git_diff produces a body; a still-running / failed one returns
// false so the dispatcher falls through to the shared Failure fallback.

#include "tool_body_common.hpp"

#include <string>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

bool git_diff_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const auto& body = tc.output();
    if (!body.empty() && body != "no changes") {
        out.kind = Kind::GitDiff;
        out.text = std::string{body};
        out.text_color = text_tertiary;
    }
    return true;
}

} // namespace agentty::ui::detail
