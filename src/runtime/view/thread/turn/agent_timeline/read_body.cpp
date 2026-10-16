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
#include <string_view>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

namespace {
// Returns {first line number, text without prefixes}, or {0, ""} when the
// body isn't numbered on every line (old transcripts, outline replies).
std::pair<int, std::string> strip_line_numbers(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    int first = 0;
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t eol = body.find('\n', pos);
        if (eol == std::string_view::npos) eol = body.size();
        std::size_t k = pos;
        while (k < eol && body[k] == ' ') ++k;
        const std::size_t d = k;
        int n = 0;
        while (k < eol && body[k] >= '0' && body[k] <= '9' && n < 100000000)
            n = n * 10 + (body[k++] - '0');
        if (k == d || k >= eol || body[k] != '\t') return {0, {}};
        if (first == 0) first = n;
        out.append(body.substr(k + 1, eol - k - 1));
        if (eol < body.size()) out.push_back('\n');
        pos = eol + 1;
    }
    return {first, std::move(out)};
}
} // namespace

bool read_body(const ToolUse& tc,
               const GrepHits* grep_hits,
               maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.is_done()) return false;   // failed → shared Failure fallback

    const std::string_view raw = card_output(tc);
    if (!raw.empty()) {
        // A cache hit is prefixed "[cached — unchanged since your last read
        // of this file]\n". That is the tool talking to the model, not file
        // text; leaving it in made it line 1 of the gutter and shifted every
        // real line number by one.
        std::string_view body_src{raw};
        if (body_src.starts_with("[cached"))
            if (const auto nl = body_src.find('\n'); nl != std::string_view::npos)
                body_src.remove_prefix(nl + 1);
        // The read tool decorates its output: a leading "SUCCESS: `sym`
        // defined at path:N (lines A–B).\n\n" header for symbol reads and a
        // trailing "\n[showing lines A-B of N …]" pagination footer. Both
        // would be NUMBERED AS FILE CONTENT by the FileRead gutter, skewing
        // every line number below them. Strip them here and mine them for
        // the TRUE first line — authoritative over the args echo (which is
        // absent for symbol reads and negative for tail reads).
        std::string_view body{body_src};
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
        // Each line carries read's "%6d\t" prefix. The card draws its own
        // gutter, so strip them; the first one is the true start line. Any
        // heads-up note above the numbered block is dropped with them.
        auto numbered = body;
        for (int guard = 0; guard < 8 && !numbered.empty(); ++guard) {
            std::size_t k = 0;
            while (k < numbered.size() && numbered[k] == ' ') ++k;
            const std::size_t d = k;
            while (k < numbered.size() && numbered[k] >= '0' && numbered[k] <= '9') ++k;
            if (k > d && k < numbered.size() && numbered[k] == '\t') break;
            const auto nl = numbered.find('\n');
            if (nl == std::string_view::npos) { numbered = {}; break; }
            numbered.remove_prefix(nl + 1);
        }
        if (auto stripped = strip_line_numbers(numbered); stripped.first > 0) {
            true_start = stripped.first;
            out.text = std::move(stripped.second);
        } else {
            out.text = std::string{body};
        }
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
