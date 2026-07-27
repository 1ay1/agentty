// Shared helpers for the per-tool body-preview renderers. See
// tool_body_common.hpp for the contract.

#include "tool_body_common.hpp"

#include <algorithm>

#include <maya/platform/io.hpp>

namespace agentty::ui::detail {

// ── Streaming-card body budget ────────────────────────────────────────
//
// THE INVARIANT (scrollback oracle, write/edit turn): while a write/edit
// card streams, its event HEADER row must stay inside the viewport. If
// the body grows tall enough to push the header into native scrollback,
// the settle's Running→Done restyle of that row (● bright → ✓ + dim on
// the tree/name cells — a STYLE-ONLY rewrite, invisible in char dumps)
// is a committed-row rewrite; maya's gate can only recover with a
// destructive HardReset on the grow frame.
//
// The budget therefore scales with the REAL terminal height instead of
// being pinned to the worst case. Fixed chrome below the header, counted
// from the oracle's 60x18 viewport dump:
//   header(1) + blank(1) + footer(1) + card bottom border(1) + gap(1)
//   + composer(6) + status bar(3) = 14 rows, +1 slack = 15.
// body_budget = term_rows − 15, floored at 3 (the proven-safe minimum at
// 18-row terminals — exactly the config the oracle passes with).
//
// query_terminal_size (not available_height): tool bodies are built at
// VIEW-BUILD time, before the render pass installs the sized
// RenderContext — available_height() would return the 24-row default.
int stream_body_budget() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    const int rows = sz.height.value > 0 ? sz.height.value : 24;
    constexpr int kChromeBelowHeader = 15;
    return std::max(3, rows - kChromeBelowHeader);
}

std::string tail_window(std::string_view s, std::size_t keep_lines) {
    if (s.empty()) return {};
    // Walk backwards counting newlines; stop after keep_lines+1 of them
    // (the +1 anchors the start of the first kept line).
    std::size_t nl_seen = 0;
    std::size_t start = s.size();
    for (std::size_t i = s.size(); i-- > 0;) {
        if (s[i] == '\n') {
            if (++nl_seen > keep_lines) { start = i + 1; break; }
        }
        if (i == 0) start = 0;
    }
    return std::string{s.substr(start)};
}

std::string head_window(std::string_view s, std::size_t keep_lines) {
    if (s.empty()) return {};
    std::size_t nl_seen = 0;
    std::size_t end = s.size();
    std::size_t total_lines = 1;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            ++total_lines;
            if (nl_seen + 1 == keep_lines && end == s.size()) end = i;
            ++nl_seen;
        }
    }
    if (total_lines <= keep_lines) return std::string{s};
    std::string out{s.substr(0, end)};
    std::size_t more = total_lines - keep_lines;
    out += "\n\xe2\x8b\xaf " + std::to_string(more)
         + (more == 1 ? " more line" : " more lines");  // ⋯
    return out;
}

std::string read_path_arg(const nlohmann::json& args) {
    if (!args.is_object()) return {};
    for (auto k : {"path", "file_path", "filepath", "filename"}) {
        if (auto it = args.find(k); it != args.end() && it->is_string())
            return it->get<std::string>();
    }
    return {};
}

// Parse `## Matches in <path>` and `### L<start>-<end>` markers out of
// agentty's grep tool output and accumulate (path → {start lines}) into
// `out`. We use the BLOCK START line as a representative match anchor
// — agentty's output groups matches with surrounding context, so the
// individual match offsets aren't recoverable from the rendered body.
// Block-start is good enough for the highlight_lines anchor; the user's
// eye lands on the right region of the file.
void accumulate_grep_hits(const std::string& output, GrepHits& out) {
    constexpr std::string_view kPathTag  = "## Matches in ";
    constexpr std::string_view kBlockTag = "### L";

    std::string current_path;
    std::size_t pos = 0;
    while (pos < output.size()) {
        const auto nl = output.find('\n', pos);
        const auto end = (nl == std::string::npos) ? output.size() : nl;
        const std::string_view line(output.data() + pos, end - pos);

        if (line.starts_with(kPathTag)) {
            current_path = std::string{line.substr(kPathTag.size())};
        } else if (!current_path.empty() && line.starts_with(kBlockTag)) {
            // Parse `### L<start>-<end>` — read digits up to '-' or end.
            std::size_t i = kBlockTag.size();
            int start = 0;
            bool got = false;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                start = start * 10 + (line[i] - '0');
                ++i;
                got = true;
            }
            if (got && start > 0)
                out[current_path].insert(start);
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

} // namespace agentty::ui::detail
