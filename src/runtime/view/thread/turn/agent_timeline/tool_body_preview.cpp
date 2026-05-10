#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui {

namespace {

// Best-effort path lookup: tools spell the field differently
// (write uses `file_path`, edit/read use `path`), and the live preview
// path may carry it in either depending on which alias the model picked.
// Returns empty string when nothing resembles a path.
[[nodiscard]] std::string read_path_arg(const nlohmann::json& args) {
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
// individual match offsets aren't recoverable from the rendered body
// (they live in structured `events` upstream that don't reach the view).
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

} // namespace

GrepHits collect_grep_hits(const Message& msg) {
    GrepHits out;
    for (const auto& tc : msg.tool_calls) {
        if (tc.name.value != "grep") continue;
        const auto& body = tc.output();
        if (body.empty()) continue;
        accumulate_grep_hits(body, out);
    }
    return out;
}

maya::ToolBodyPreview::Config tool_body_preview_config(
    const ToolUse& tc, const GrepHits* grep_hits)
{
    using Kind = maya::ToolBodyPreview::Kind;
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // ── Edit: parse hunks from args. (Stays EditDiff regardless of
    //    failure — the diff is the user's mental model of "what we tried
    //    to change," even when application failed.)
    if (n == "edit" && tc.args.is_object()) {
        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            out.kind = Kind::EditDiff;
            out.hunks.reserve(it->size());
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                out.hunks.push_back({std::move(ot), std::move(nt)});
            }
            return out;
        }
        // Top-level legacy single-edit shape.
        auto ot = safe_arg(tc.args, "old_text");
        if (ot.empty()) ot = safe_arg(tc.args, "old_string");
        auto nt = safe_arg(tc.args, "new_text");
        if (nt.empty()) nt = safe_arg(tc.args, "new_string");
        if (!ot.empty() || !nt.empty()) {
            out.kind = Kind::EditDiff;
            out.hunks.push_back({std::move(ot), std::move(nt)});
        }
        return out;
    }

    // ── Bash / diagnostics: BashOutput (tail-only, structured-extraction
    //    fallback chain in maya picks up gtest-style `N tests passed`
    //    summaries and compiler-error rows, otherwise falls back to a
    //    dim 4-row tail). `failed` is wired for the inline `· exit N`
    //    suffix on the last line of failed output. We keep BashOutput
    //    on failure (instead of routing to Kind::Failure) so the timeline
    //    card border + status icon carry the failure signal and the body
    //    stays calm — agent_session.cpp's "no double-flagging" discipline.
    if (n == "bash" || n == "diagnostics") {
        if (tc.is_running() && !tc.progress_text().empty()) {
            out.kind = Kind::BashOutput;
            out.text = tc.progress_text();
            out.text_color = fg;
            out.is_streaming = true;
            return out;
        }
        if (tc.is_terminal()) {
            auto stripped = strip_bash_output_fence(tc.output());
            if (!stripped.empty()) {
                out.kind = Kind::BashOutput;
                out.text = std::move(stripped);
                out.text_color = fg;
                out.failed = tc.is_failed();
            }
            return out;
        }
        return out;   // pending/approved with no body — render nothing
    }

    // ── Write: FileWrite (subtle "+" prefix + lines/bytes footer). Keeps
    //    the byte-count signal that's the whole reason a Write event has
    //    a body — the path is in the timeline header.
    if (n == "write") {
        auto content = safe_arg(tc.args, "content");
        if (!content.empty()) {
            out.kind = Kind::FileWrite;
            out.text = std::move(content);
            out.text_color = fg;
            out.show_footer_stats = true;
            // While args are streaming and the model hasn't started
            // emitting `content` yet, BashOutput's empty-streaming path
            // applies here too — but FileWrite renders nothing for
            // empty text by default and the header spinner suffices.
        } else if (tc.is_running()) {
            out.kind = Kind::FileWrite;
            out.text_color = fg;
            out.is_streaming = true;
        }
        return out;
    }

    // ── git_diff: per-line +/-/@@ coloring. Stays GitDiff.
    if (n == "git_diff" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty() && body != "no changes") {
            out.kind = Kind::GitDiff;
            out.text = body;
            out.text_color = fg;
        }
        return out;
    }

    // ── Read / find_definition: FileRead with line gutter. When a
    //    preceding Grep on the same path produced hits, inherit them as
    //    highlight_lines so the read body anchors the user's eye on the
    //    relevant region instead of forcing a re-scan. The summary header
    //    `▸ matches: N1, N2, …` lists every hit even when they fall
    //    outside the rendered head budget — common in long files where
    //    the matches live mid-file but the read body shows the top.
    if ((n == "read" || n == "find_definition") && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty()) {
            out.kind = Kind::FileRead;
            out.text = body;
            out.text_color = fg;
            if (grep_hits) {
                if (auto path = read_path_arg(tc.args); !path.empty()) {
                    if (auto it = grep_hits->find(path); it != grep_hits->end())
                        out.highlight_lines = it->second;
                }
            }
        }
        return out;
    }
    if ((n == "read" || n == "find_definition") && tc.is_failed()
        && !tc.output().empty())
    {
        // Read failures (file not found, permission denied) read
        // naturally as red text — no FileRead gutter would make sense
        // for an error message. Fall through to the explicit Failure
        // path below.
    }

    // ── web_fetch: Json (self-sniffs and falls back to CodeBlock when
    //    the body isn't JSON, so it's safe as the unconditional pick).
    if (n == "web_fetch" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty()) {
            out.kind = Kind::Json;
            out.text = body;
            out.text_color = fg;
        }
        return out;
    }

    // ── Generic line-oriented tools that DON'T have a structured body
    //    Kind: head+tail CodeBlock preview. agentty's grep emits markdown
    //    (`## Matches in <path>` / `### L<s>-<e>` blocks) rather than
    //    the raw `path:line:text` shape that maya::Kind::GrepMatches
    //    parses, so it stays on CodeBlock here. (The cross-tool grep_hits
    //    index above still picks up the line anchors for FileRead.)
    if ((n == "grep" || n == "glob" || n == "list_dir"
         || n == "web_search"
         || n == "git_status" || n == "git_log" || n == "git_commit")
        && tc.is_done())
    {
        if (!tc.output().empty()) {
            out.kind = Kind::CodeBlock;
            out.text = tc.output();
            out.text_color = fg;
        }
        return out;
    }

    // ── Failure fallback for everything else: surface stderr as red.
    //    Routed AFTER per-tool Kinds so bash/write/etc. can use their
    //    own kind even on failure (preventing double-flagging against
    //    the card's red border + status icon).
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = tc.output();
        return out;
    }

    // ── Todo: parse items + statuses.
    if (n == "todo" && tc.args.is_object()) {
        if (auto it = tc.args.find("todos");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            using Status = maya::ToolBodyPreview::TodoItem::Status;
            out.kind = Kind::TodoList;
            out.todos.reserve(it->size());
            for (const auto& td : *it) {
                if (!td.is_object()) continue;
                Status s = Status::Pending;
                auto st = td.value("status", std::string{"pending"});
                if      (st == "completed")   s = Status::Completed;
                else if (st == "in_progress") s = Status::InProgress;
                out.todos.push_back({td.value("content", ""), s});
            }
            return out;
        }
    }

    return out;     // kind = None
}

} // namespace agentty::ui
