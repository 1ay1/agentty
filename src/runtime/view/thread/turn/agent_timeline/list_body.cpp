// tool_body_preview — generic line-oriented tools:
// grep / glob / list_dir / web_search / git_status / git_log / git_commit.
//
// These DON'T have a structured body Kind, so they render a head+tail
// CodeBlock preview. agentty's grep emits markdown (`## Matches in <path>`
// / `### L<s>-<e>` blocks) rather than the raw `path:line:text` shape that
// maya::Kind::GrepMatches parses, so it stays on CodeBlock here. (The
// cross-tool grep_hits index still picks up the line anchors for FileRead.)
//
// Body renders in text_tertiary (dim) — the category color lives on the
// header NAME where it carries the visual identity; making bodies colorful
// too creates noise and competes with the prose-style markdown above the
// tool group. Only a DONE tool produces a body; otherwise returns false to
// fall through to the shared Failure fallback.

#include "tool_body_common.hpp"

#include <string>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

bool generic_list_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    if (!tc.output().empty()) {
        out.kind = Kind::CodeBlock;
        out.text = std::string{tc.output()};
        out.text_color = text_tertiary;
    }
    return true;
}

} // namespace agentty::ui::detail
