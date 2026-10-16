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

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

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

// Tells which rows of a grep block are hits. grep's markdown doesn't mark
// them, so re-test each row with the call's own pattern, using the same
// case/word rules and the same literal fallback for a bad regex.
class HitTest {
public:
    explicit HitTest(const nlohmann::json& args) {
        pat_ = safe_arg(args, "pattern");
        icase_ = !(args.is_object() && args.contains("case_sensitive")
                   && args["case_sensitive"].is_boolean()
                   && args["case_sensitive"].get<bool>());
        const bool word = args.is_object() && args.contains("word")
                       && args["word"].is_boolean() && args["word"].get<bool>();
        if (pat_.empty()) return;
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (icase_) flags |= std::regex::icase;
        if (auto ex = safe_arg(args, "exclude"); !ex.empty()) {
            try { ex_.emplace(ex, flags); } catch (const std::regex_error&) {}
        }
        try {
            re_.emplace(word ? "\\b(?:" + pat_ + ")\\b" : pat_, flags);
        } catch (const std::regex_error&) {
            re_.reset();   // searched as text, like the tool does
        }
    }
    [[nodiscard]] bool usable() const { return !pat_.empty(); }
    [[nodiscard]] bool hit(std::string_view row) const {
        if (row.size() > 4096) row = row.substr(0, 4096);   // bound regex cost
        if (ex_) {
            try { if (std::regex_search(row.begin(), row.end(), *ex_)) return false; }
            catch (const std::regex_error&) {}
        }
        if (re_) {
            try { return std::regex_search(row.begin(), row.end(), *re_); }
            catch (const std::regex_error&) { return false; }
        }
        if (!icase_) return row.find(pat_) != std::string_view::npos;
        auto lo = [](unsigned char c) { return std::tolower(c); };
        return std::search(row.begin(), row.end(), pat_.begin(), pat_.end(),
                           [&](char a, char b) { return lo(a) == lo(b); })
               != row.end();
    }
private:
    std::string pat_;
    bool icase_ = true;
    std::optional<std::regex> re_;
    std::optional<std::regex> ex_;
};

} // namespace

bool grep_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    const std::string_view raw = card_output(tc);
    if (raw.empty()) return true;

    // Walk the markdown shape and synthesize `path:line:text` rows. `rows`
    // keeps every row; `hits` only the ones the pattern matches, which is
    // what the card shows (a 1-match block with 20 lines of context used to
    // preview its first 8 context lines and never the match).
    const HitTest test{tc.args};
    std::string rows, hits;
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
                std::string row{current_path};
                row += ':';
                row += std::to_string(next_line++);
                row += ':';
                row += line;
                row += '\n';
                if (test.usable() && test.hit(line)) hits += row;
                rows += row;
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
    // No row matched card-side (a regex feature std::regex lacks, a
    // multiline pattern): show every row rather than nothing.
    out.text = hits.empty() ? std::move(rows) : std::move(hits);
    out.text_color = text_tertiary;
    return true;
}

bool generic_list_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;
    if (!card_output(tc).empty()) {
        out.kind = Kind::CodeBlock;
        out.text = std::string{card_output(tc)};
        out.text_color = text_tertiary;
        // Head-heavy output: show the FIRST lines (plus a small tail via
        // the head_tail profile) instead of the tail-anchored window that
        // surfaced only the pagination footer.
        out.tail_only = false;
    }
    return true;
}

} // namespace agentty::ui::detail
