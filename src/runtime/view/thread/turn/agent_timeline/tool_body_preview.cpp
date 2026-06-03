#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include <string>
#include <string_view>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

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

// During streaming the body grows by a delta every ~120ms and the view
// rebuilds the whole Turn every frame. Handing maya the FULL content
// means: (a) an O(N) std::move copy into the Config, and (b) maya's
// split_lines/elide/count_lines each walk the entire body — all to
// render the last few tail lines. Slice to a bounded tail window on our
// side so per-frame cost is O(window), not O(file). maya's FileWrite /
// CodeBlock / FileRead renderers are tail-anchored (show_all=false →
// last code_tail lines), so the visible output is byte-identical to
// feeding the full body. Keep a generous margin (kStreamTailLines) so
// the widget's tail budget is always satisfied. When the tool goes
// terminal the caller feeds the full content (show_all=true) and the
// final card renders everything.
constexpr std::size_t kStreamTailLines = 64;

// Live-tail window budget for line-oriented bodies (FileRead,
// CodeBlock, GitDiff, Json, web_fetch). With show_all=false and
// tail_only=true (maya's defaults for these kinds) the widget renders
// only the last max(head,tail) lines — so feeding the full body makes
// split_lines/elide walk O(file) every frame for a fixed-size preview.
// A tail slice generous enough to cover every renderer's tail budget
// keeps the visible output byte-identical while bounding per-frame cost
// to O(window). The frozen snapshot (built under FrozenBuildScope) still
// gets the full body — painted once, then blitted.
constexpr std::size_t kLiveTailLines = 64;

[[nodiscard]] std::string tail_window(std::string_view s,
                                      std::size_t keep_lines) {
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

// Body for a terminal line-oriented tool sitting in the LIVE tail.
// Frozen builds keep the full body (full content, painted once); the
// live path elides to a bounded tail window so per-frame split_lines
// stays O(window) instead of O(file). Cheap no-op for short bodies
// (tail_window returns the whole string when it has <= keep_lines).
[[nodiscard]] std::string live_tail_body(std::string_view body) {
    if (building_frozen()) return std::string{body};
    return tail_window(body, kLiveTailLines);
}

} // namespace

// ── Frozen-build scope ──────────────────────────────────────────────
// A terminal write/edit/read card that lives in the LIVE tail (an
// in-flight run whose earlier sub-turn already finished a big card) is
// re-rendered every frame until the run settles and freeze_range
// snapshots it. Rendering its FULL body each frame is the dominant live
// cost (a 3000-line read measured ~21ms/frame). The frozen snapshot is
// painted once and blitted, so it keeps the full body; the live render
// elides to a window. This thread-local says which phase we're in.
namespace {
bool& frozen_build_flag() noexcept {
    thread_local bool v = false;
    return v;
}
} // namespace

FrozenBuildScope::FrozenBuildScope() noexcept
    : prev_(frozen_build_flag()) { frozen_build_flag() = true; }
FrozenBuildScope::~FrozenBuildScope() { frozen_build_flag() = prev_; }
bool building_frozen() noexcept { return frozen_build_flag(); }

GrepHits collect_grep_hits(std::span<const ToolUse> tool_calls) {
    GrepHits out;
    for (const auto& tc : tool_calls) {
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

    // Chrome (line-number gutter, pipe separator, elision marker) reads
    // in the tool's category color so the body's structure visually
    // matches the header NAME color. Body content itself stays
    // text_tertiary (dim) so the colored chrome frames it without
    // competing with prose.
    out.chrome_color = tool_category_color(n);

    // ── Edit
    //
    //  Two rendering paths, picked by what data we have:
    //
    //  (a) FENCE-PARSE — for terminal-state edits, pull the diff
    //      payload out of the ```diff … ``` block the edit tool writes
    //      into its output text. Routes through Kind::GitDiff for
    //      interleaved −/+ coloring + line anchors.
    //
    //  (b) ARGS-ECHO — streaming / pre-run, no diff exists yet:
    //      synthesize a Kind::EditDiff from `tc.args.edits[*]`. The
    //      file's `before` content isn't reachable from the view so
    //      this is the best preview we can offer until execution
    //      lands.
    if (n == "edit") {
        const bool streaming_now = !tc.is_terminal();
        if (tc.is_terminal() && !tc.is_failed()) {
            // Pull the diff payload out of the ```diff … ``` fence in the
            // tool's output. Falls back to EditDiff-from-args if the fence
            // isn't found (older threads, custom tool wrappers).
            const auto& body = tc.output();
            constexpr std::string_view kOpen = "```diff\n";
            constexpr std::string_view kClose = "\n```";
            auto a = body.find(kOpen);
            if (a != std::string::npos) {
                a += kOpen.size();
                auto b = body.find(kClose, a);
                if (b == std::string::npos) b = body.size();
                out.kind = Kind::GitDiff;
                // Full diff as soon as the edit is terminal — in the live
                // tail too, not only the frozen snapshot. The output is
                // FINAL the instant the tool settles, so eliding to a
                // tail window here just to expand it one tick later (when
                // freeze_range rebuilds with show_all) is the visible
                // "diff squeezes, then pops to full size" lag. Per-frame
                // split_lines over a stable body for the one-or-two
                // frames before the freeze handoff is negligible, and the
                // frozen snapshot emits the same full body so the handoff
                // is seamless (no height jump).
                out.text     = body.substr(a, b - a);
                out.show_all = true;
                out.text_color = text_tertiary;
                return out;
            }
            // Fence missing — fall through to args-based EditDiff below.
        }

        if (tc.args.is_object()) {
            // Mirror Write's discipline: while the edit is streaming, the
            // hunks grow line-by-line (each `edits[i].new_text` delta
            // arrives mid-token), which would balloon the card height on
            // every frame and fragment any rows already pushed to native
            // scrollback. Pin the streaming preview to the tail window
            // (show_all=false → maya's tail_only renderer shows just the
            // last N lines per hunk side); expand to show_all only once
            // the tool has settled and the body is final.
            if (auto it = tc.args.find("edits");
                it != tc.args.end() && it->is_array() && !it->empty())
            {
                out.kind = Kind::EditDiff;
                // Full diff as soon as the edit is terminal — in the live
                // tail too, not only the frozen snapshot. While STREAMING
                // keep the elided per-side/per-hunk preview (hunks grow
                // line-by-line, would balloon height every frame); once
                // settled the hunks are final, so expanding immediately
                // avoids the "stub then sudden expand" lag and matches
                // what freeze_range will build (seamless handoff).
                out.show_all     = !streaming_now;
                out.is_streaming = streaming_now;
                out.hunks.reserve(it->size());
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    auto ot = e.value("old_text", e.value("old_string", std::string{}));
                    auto nt = e.value("new_text", e.value("new_string", std::string{}));
                    out.hunks.push_back({std::move(ot), std::move(nt)});
                }
                return out;
            }
            auto ot = safe_arg(tc.args, "old_text");
            if (ot.empty()) ot = safe_arg(tc.args, "old_string");
            auto nt = safe_arg(tc.args, "new_text");
            if (nt.empty()) nt = safe_arg(tc.args, "new_string");
            if (!ot.empty() || !nt.empty()) {
                out.kind = Kind::EditDiff;
                out.show_all     = !streaming_now;
                out.is_streaming = streaming_now;
                out.hunks.push_back({std::move(ot), std::move(nt)});
            }
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
    // ── Bash / diagnostics: BashOutput (tail-oriented, with `· exit N`
    //    on failure).
    if (n == "bash" || n == "diagnostics") {
        if (tc.is_running() && !tc.progress_text().empty()) {
            out.kind = Kind::BashOutput;
            out.text = tc.progress_text();
            out.text_color = text_tertiary;
            out.is_streaming = true;
            return out;
        }
        if (tc.is_terminal()) {
            auto stripped = strip_bash_output_fence(tc.output());
            if (!stripped.empty()) {
                out.kind = Kind::BashOutput;
                out.text = std::move(stripped);
                out.text_color = text_tertiary;
                out.failed = tc.is_failed();
            }
            return out;
        }
        return out;
    }

    // ── Write: FileWrite (subtle "+" prefix + lines/bytes footer). Keeps
    //    the byte-count signal that's the whole reason a Write event has
    //    a body — the path is in the timeline header.
    // ── Write: line-numbered body + lines/bytes footer.
    //    show_all is set so the full new-file content is rendered (no
    //    "⋯ N more" elision); the user wants to see exactly what was
    //    written.
    if (n == "write") {
        auto content = safe_arg(tc.args, "content");
        const bool streaming_now = !tc.is_terminal();
        if (!content.empty()) {
            out.kind = Kind::FileWrite;
            out.text_color = text_tertiary;
            out.show_footer_stats = true;
            // While streaming, show a SMALL tail preview (the last few
            // lines of what's been written so far) — show_all=false lets
            // maya elide to its `code_tail` budget, the compact "watch it
            // write" look. The duplicated-write ghost came from feeding a
            // LARGE tail slice (64 lines): once those rows overflowed into
            // native scrollback they were frozen, but on settle the card
            // switched to a head-anchored show_all render from line 1, so
            // the committed rows no longer matched and the card was
            // re-emitted below them (two copies). Keeping the streaming
            // preview SMALL (a slice barely above maya's tail budget)
            // keeps it inside the live viewport so it never commits to
            // scrollback — only the settled show_all render reaches
            // scrollback, and it's painted once.
            out.show_all     = !streaming_now;
            out.is_streaming = streaming_now;
            if (streaming_now) {
                // Small tail slice → O(window) per frame and a fixed
                // compact card height. show_all=false above makes maya
                // render just its `code_tail` lines from this slice.
                // Footer totals would be wrong on a partial body, so
                // suppress mid-stream (status bar carries the live rate);
                // it returns with the true total the instant we settle.
                out.text = tail_window(content, kStreamTailLines);
                out.show_footer_stats = false;
            } else {
                // Terminal: full body, show_all, in BOTH the live tail and
                // the frozen snapshot — identical bytes/height across the
                // freeze instant. This is the only write render that ever
                // commits to native scrollback, so there is nothing above
                // it to disagree with.
                out.text = std::move(content);
            }
        } else if (tc.is_running()) {
            out.kind = Kind::FileWrite;
            out.text_color = text_tertiary;
            out.is_streaming = true;
        }
        return out;
    }

    // ── git_diff: per-line +/-/@@ coloring (GitDiff owns the palette).
    if (n == "git_diff" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty() && body != "no changes") {
            out.kind = Kind::GitDiff;
            out.text = live_tail_body(body);
            out.text_color = text_tertiary;
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
            out.text = live_tail_body(body);
            out.text_color = text_tertiary;    // bright cyan — file content rendered as code
            // Anchor the gutter to the real source line numbers the tool
            // returned, not 1. The read tool accepts both `offset` and the
            // Zed-style alias `start_line` (see tools/read.cpp
            // parse_args); either lands in the args JSON verbatim. When
            // neither is set the file was read from the top, so the
            // default start_line=1 in the widget Config is already right.
            if (tc.args.is_object()) {
                for (auto k : {"start_line", "offset"}) {
                    if (auto it = tc.args.find(k);
                        it != tc.args.end() && it->is_number_integer())
                    {
                        int v = it->get<int>();
                        if (v >= 1) { out.start_line = v; break; }
                    }
                }
            }
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

    // ── web_fetch: Json (pretty-printed key/value).
    if (n == "web_fetch" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty()) {
            out.kind = Kind::Json;
            out.text = live_tail_body(body);
            out.text_color = text_tertiary;
        }
        return out;
    }

    // ── Generic line-oriented tools that DON'T have a structured body
    //    Kind: head+tail CodeBlock preview. agentty's grep emits markdown
    //    (`## Matches in <path>` / `### L<s>-<e>` blocks) rather than
    //    the raw `path:line:text` shape that maya::Kind::GrepMatches
    //    parses, so it stays on CodeBlock here. (The cross-tool grep_hits
    //    index above still picks up the line anchors for FileRead.)
    //
    //    Body renders in `text_secondary` (legible mid-gray, ANSI 7)
    //    across all kinds — the category color lives on the header NAME
    //    where it carries the visual identity; making bodies colorful
    //    too creates noise and competes with the prose-style markdown
    //    above the tool group. Tool metadata (name / detail / elapsed)
    //    stays bright; tool output recedes to subordinate gray.
    // ── Generic line-oriented tools: CodeBlock head+tail preview.
    if ((n == "grep" || n == "glob" || n == "list_dir"
         || n == "web_search"
         || n == "git_status" || n == "git_log" || n == "git_commit")
        && tc.is_done())
    {
        if (!tc.output().empty()) {
            out.kind = Kind::CodeBlock;
            out.text = live_tail_body(tc.output());
            out.text_color = text_tertiary;
        }
        return out;
    }

    // ── Failure fallback. Chrome flips to red so the body chrome
    //    matches the card's failure cue; body content stays dim.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = live_tail_body(tc.output());
        out.chrome_color = status_error;
        return out;
    }

    // ── Todo: structured checkbox list (TodoList owns the icons).
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
