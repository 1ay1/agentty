// tool_body_preview — `replace` / `rewrite_structural` dry-run + applied
// preview bodies.
//
// Both tools emit a per-hit `-old` / `+new` preview with real line numbers,
// but NOT in unified-diff shape (leading indent, a `path:` or `L<n>:`
// prefix), so maya's GitDiff parser — which keys off a line-leading `+`/`-`
// — won't colour them as-is. This reshapes each preview into the minimal
// unified-diff GitDiff understands:
//
//   replace:              rewrite_structural:
//     src/a.cpp:12 (2 hits)   src/a.cpp:
//       - foo(bar)              L12: - foo(bar)
//       + foo(baz)              L12: + foo(baz)
//
//   →  +++ b/src/a.cpp
//      @@ 12 @@              (a bare line-number anchor GitDiff shows dim)
//      -foo(bar)
//      +foo(baz)
//
// The leading summary line ("DRY RUN — would replace …") is dropped: the
// count already rides the event header. Falls back to a dim CodeBlock when
// the output has no parseable -/+ preview (e.g. the "no matches" message).

#include "tool_body_common.hpp"

#include <string>
#include <string_view>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

namespace {

// Pull a leading `L<digits>:` off a rewrite_structural preview line body,
// returning the line number (or 0) and advancing `s` past it.
int take_line_prefix(std::string_view& s) {
    auto t = s;
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    if (t.empty() || t.front() != 'L') return 0;
    t.remove_prefix(1);
    int n = 0;
    bool got = false;
    while (!t.empty() && t.front() >= '0' && t.front() <= '9') {
        n = n * 10 + (t.front() - '0');
        t.remove_prefix(1);
        got = true;
    }
    if (!got || t.empty() || t.front() != ':') return 0;
    t.remove_prefix(1);
    s = t;               // consumed `L<n>:`
    return n;
}

// Is this a `path:` or `path:line  (N hits)` file-header line? Returns the
// path (without the trailing `:` / count) when so.
std::string_view file_header(std::string_view line) {
    // Reject lines that are clearly -/+ previews or gap markers.
    auto t = line;
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    if (t.empty()) return {};
    if (t.front() == '-' || t.front() == '+' || t.front() == 'L') return {};
    // A header ends in `:` optionally followed by `<line>  (N hit[s])`.
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return {};
    // Everything before the first colon is the path (paths here never
    // contain a colon — they're workspace-relative).
    return line.substr(0, colon);
}

} // namespace

bool diff_preview_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const auto& raw = tc.output();
    if (raw.empty()) return true;

    std::string diff;
    diff.reserve(raw.size());
    std::string_view current_path;
    int pending_line = 0;
    bool any = false;

    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto nl = raw.find('\n', pos);
        const auto end = (nl == std::string_view::npos) ? raw.size() : nl;
        std::string_view line(raw.data() + pos, end - pos);
        pos = (nl == std::string_view::npos) ? raw.size() : nl + 1;

        // Trim a single trailing '\r'.
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        // File header?
        if (auto path = file_header(line); !path.empty()) {
            current_path = path;
            diff += "+++ b/";
            diff += path;
            diff += '\n';
            // If the header carries a line number (replace: `path:line  …`),
            // emit an anchor so the first hunk shows its position.
            auto colon = line.find(':');
            int hl = 0;
            for (std::size_t i = colon + 1; i < line.size()
                 && line[i] >= '0' && line[i] <= '9'; ++i)
                hl = hl * 10 + (line[i] - '0');
            pending_line = hl;
            continue;
        }

        // Preview body line: optional indent, optional `L<n>:`, then -/+.
        std::string_view body = line;
        int lno = take_line_prefix(body);   // rewrite_structural
        while (!body.empty() && body.front() == ' ') body.remove_prefix(1);
        if (body.empty()) continue;

        if (body.front() == '-' || body.front() == '+') {
            const char sign = body.front();
            body.remove_prefix(1);
            if (!body.empty() && body.front() == ' ') body.remove_prefix(1);
            // Emit a hunk anchor before the FIRST '-' of a group so the
            // reader sees the line number. replace carries it on the header
            // (pending_line); rewrite carries it per-line (lno).
            if (sign == '-') {
                int at = lno ? lno : pending_line;
                if (at > 0) {
                    diff += "@@ L";
                    diff += std::to_string(at);
                    diff += " @@\n";
                }
                pending_line = 0;
            }
            diff += sign;
            diff += body;
            diff += '\n';
            any = true;
        }
        // Everything else (summary line, "[capped]", gap notes) is skipped.
    }

    if (!any) {
        // No parseable preview — keep the message readable, head-anchored.
        out.kind = Kind::CodeBlock;
        out.text = std::string{raw};
        out.text_color = text_tertiary;
        out.tail_only = false;
        return true;
    }
    out.kind = Kind::GitDiff;
    out.text = std::move(diff);
    out.text_color = text_tertiary;
    return true;
}

} // namespace agentty::ui::detail
