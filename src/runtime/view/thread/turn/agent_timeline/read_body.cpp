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

    const auto& raw = tc.output();
    if (!raw.empty()) {
        // The read tool decorates its output: a leading "SUCCESS: `sym`
        // defined at path:N (lines A–B).\n\n" header for symbol reads and a
        // trailing "\n[showing lines A-B of N …]" pagination footer. Both
        // would be NUMBERED AS FILE CONTENT by the FileRead gutter, skewing
        // every line number below them. Strip them here and mine them for
        // the TRUE first line — authoritative over the args echo (which is
        // absent for symbol reads and negative for tail reads).
        std::string_view body{raw};
        int true_start = 0;

        auto parse_int_at = [](std::string_view s, std::size_t i) -> int {
            int v = 0; bool got = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + (s[i] - '0'); ++i; got = true;
            }
            return got ? v : 0;
        };

        // Footer: authoritative for offset/limit and tail (-N) reads.
        constexpr std::string_view kFooterTag = "\n[showing lines ";
        if (auto fp = body.rfind(kFooterTag); fp != std::string_view::npos) {
            true_start = parse_int_at(body, fp + kFooterTag.size());
            body = body.substr(0, fp);
        }
        // Symbol header: "SUCCESS: `x` defined at f:N (lines A–B).\n\n".
        constexpr std::string_view kSymTag = "SUCCESS: `";
        if (body.starts_with(kSymTag)) {
            if (auto he = body.find("\n\n"); he != std::string_view::npos) {
                constexpr std::string_view kLines = "(lines ";
                if (auto lp = body.find(kLines); lp < he)
                    true_start = parse_int_at(body, lp + kLines.size());
                body = body.substr(he + 2);
            }
        }

        out.kind = Kind::FileRead;
        out.text = std::string{body};
        out.text_color = text_tertiary;    // bright cyan — file content rendered as code
        // Anchor the gutter to the real source line numbers. Priority:
        // the number mined from the tool's own footer / symbol header
        // (ground truth — covers symbol reads and tail reads), then the
        // `offset` / `start_line` arg echo, then the default 1.
        if (true_start >= 1) {
            out.start_line = true_start;
        } else if (tc.args.is_object()) {
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
