// tool_body_preview — `read` / `find_definition` tool body.
//
// FileRead with a line gutter. When a preceding Grep on the same path
// produced hits, inherit them as highlight_lines so the read body anchors
// the user's eye on the relevant region instead of forcing a re-scan. The
// summary header `▸ matches: N1, N2, …` lists every hit even when they
// fall outside the rendered head budget — common in long files where the
// matches live mid-file but the read body shows the top.
//
// A DONE read produces the FileRead body (returns true). A FAILED read
// returns false so the dispatcher's shared Failure fallback renders the
// error text as plain red — no FileRead gutter would make sense for an
// error message.

#include "tool_body_common.hpp"

#include <string>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

bool read_body(const ToolUse& tc,
               const GrepHits* grep_hits,
               maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;   // failed → shared Failure fallback

    const auto& body = tc.output();
    if (!body.empty()) {
        out.kind = Kind::FileRead;
        out.text = std::string{body};
        out.text_color = text_tertiary;    // bright cyan — file content rendered as code
        // Anchor the gutter to the real source line numbers the tool
        // returned, not 1. The read tool accepts both `offset` and the
        // Zed-style alias `start_line` (see tools/read.cpp parse_args);
        // either lands in the args JSON verbatim. When neither is set the
        // file was read from the top, so the default start_line=1 in the
        // widget Config is already right.
        if (tc.args.is_object()) {
            for (auto k : {"start_line", "offset"}) {
                const int v = safe_int_arg(tc.args, k, 0);
                if (v >= 1) { out.start_line = v; break; }
            }
        }
        if (grep_hits) {
            if (auto path = read_path_arg(tc.args); !path.empty()) {
                if (auto it = grep_hits->find(path); it != grep_hits->end())
                    out.highlight_lines = it->second;
            }
        }
    }
    return true;
}

} // namespace agentty::ui::detail
