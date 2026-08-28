// tool_body_preview — generic line-oriented tools:
// grep / glob / list_dir / web_search / git_status / git_log / git_commit.
//
// grep gets a STRUCTURED body: agentty's grep emits markdown
// (`## Matches in <path>` headers, `### [sym ›] L<s>-<e>` block tags,
// fenced code rows). grep_body() re-derives per-row line numbers from the
// block tags (rows inside a block are contiguous, so start+index is exact)
// and synthesizes the canonical `path:line:text` shape that
// maya::Kind::GrepMatches renders as a grouped path → right-aligned
// line-number table. The cross-tool grep_hits index (tool_body_common)
// parses the same markdown for FileRead highlights.
//
// The remaining tools render a head+tail CodeBlock preview with
// tail_only OFF: their output is HEAD-heavy (top-ranked files, first
// entries, freshest commits) and ends with pagination/truncation chatter,
// so a tail-anchored window showed exactly the wrong half.
//
// Body renders in text_tertiary (dim) — the category color lives on the
// header NAME where it carries the visual identity; making bodies colorful
// too creates noise and competes with the prose-style markdown above the
// tool group. Only a DONE tool produces a body; otherwise returns false to
// fall through to the shared Failure fallback.

#include "tool_body_common.hpp"

#include <string>
#include <string_view>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui::detail {

namespace {

// Parse the trailing `L<start>` out of a `### [breadcrumb ›] L<s>-<e>` block
// tag. Returns 0 when the tag doesn't carry one.
int block_start_line(std::string_view tag) {
    // The line-range marker is the LAST `L<digits>` run on the line (a
    // breadcrumb could itself contain "L"; the range is always terminal).
    for (std::size_t i = tag.size(); i-- > 0;) {
        if (tag[i] != 'L') continue;
        std::size_t j = i + 1;
        int v = 0;
        bool got = false;
        while (j < tag.size() && tag[j] >= '0' && tag[j] <= '9') {
            v = v * 10 + (tag[j] - '0');
            ++j;
            got = true;
        }
        if (got && (j >= tag.size() || tag[j] == '-')) return v;
    }
    return 0;
}

} // namespace

bool grep_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const auto& raw = tc.output();
    if (raw.empty()) return true;

    // Walk the markdown shape and synthesize `path:line:text` rows.
    std::string rows;
    rows.reserve(raw.size());
    std::string_view current_path;
    int  next_line = 0;
    bool in_fence  = false;

    constexpr std::string_view kPathTag  = "## Matches in ";
    constexpr std::string_view kBlockTag = "### ";

    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto nl  = raw.find('\n', pos);
        const auto end = (nl == std::string::npos) ? raw.size() : nl;
        const std::string_view line(raw.data() + pos, end - pos);
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;

        if (in_fence) {
            if (line.starts_with("```")) { in_fence = false; continue; }
            if (!current_path.empty() && next_line > 0) {
                rows += current_path;
                rows += ':';
                rows += std::to_string(next_line++);
                rows += ':';
                rows += line;
                rows += '\n';
            }
            continue;
        }
        if (line.starts_with(kPathTag)) {
            current_path = line.substr(kPathTag.size());
        } else if (line.starts_with(kBlockTag)) {
            next_line = block_start_line(line);
        } else if (line.starts_with("```")) {
            in_fence = true;
        }
        // Headline ("Found N matches…"), pagination trailer and blank
        // separators are dropped: the match COUNT already lives in the
        // event header line, and GrepMatches' own "⋯ N more" marker covers
        // truncation.
    }

    if (rows.empty()) {
        // No parsable fences (no-matches message, size-cap notice, an MCP
        // grep with a different shape) — keep the raw text readable.
        out.kind = Kind::CodeBlock;
        out.text = std::string{raw};
        out.text_color = text_tertiary;
        out.tail_only = false;
        return true;
    }
    out.kind = Kind::GrepMatches;
    out.text = std::move(rows);
    out.text_color = text_tertiary;
    return true;
}

bool generic_list_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    if (!tc.output().empty()) {
        out.kind = Kind::CodeBlock;
        out.text = std::string{tc.output()};
        out.text_color = text_tertiary;
        // Head-heavy output: show the FIRST lines (plus a small tail via
        // the head_tail profile) instead of the tail-anchored window that
        // surfaced only the pagination footer.
        out.tail_only = false;
    }
    return true;
}

} // namespace agentty::ui::detail
