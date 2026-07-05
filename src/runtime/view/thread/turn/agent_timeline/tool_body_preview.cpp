#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

#include <maya/platform/io.hpp>

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
// RenderContext — available_height() would return the 24-row default
// (see welcome_screen.hpp's max_rows rationale; pickers.cpp uses the
// same direct query for the same reason).
[[nodiscard]] int stream_body_budget() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    const int rows = sz.height.value > 0 ? sz.height.value : 24;
    constexpr int kChromeBelowHeader = 15;
    return std::max(3, rows - kChromeBelowHeader);
}

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

// Keep the FIRST keep_lines lines; if more follow, append a dim
// "\xe2\x8b\xaf N more lines" marker so the body has a hard row ceiling. Used by
// the subagent card — a long report would otherwise dominate the timeline;
// the head is the outcome line + the most important details, and the full
// report is always available verbatim in the wire/transcript.
[[nodiscard]] std::string head_window(std::string_view s,
                                      std::size_t keep_lines) {
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

} // namespace

// ── Frozen-build scope ─────────────────────────────────────
// Retained as a no-op-by-default phase flag for callers that still ask
// `building_frozen()`. The body is now IDENTICAL in both phases (full,
// seam-safe — see the comment above tool_body_preview_config), so no
// renderer path branches on it for content; freeze_range still scopes it
// for clarity and so a future divergent-build path has a hook.
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
                // Settled edit ALWAYS renders the full diff (show_all) — in
                // the live tail AND the frozen snapshot, byte-identical.
                // The user reviews exactly what changed; the per-event
                // hash_id cell-cache (agent_timeline.cpp) makes the tall
                // card a paint-once blit even while it sits in the live
                // tail, so a full body costs nothing per frame after the
                // first. Live == frozen body => the freeze handoff is a
                // pure cache hit (no committed-row shift).
                out.text       = std::string{body.substr(a, b - a)};
                out.show_all   = true;
                out.tail_only  = false;
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
                out.is_streaming = streaming_now;
                out.hunks.reserve(it->size());
                for (const auto& e : *it) {
                    if (!e.is_object()) continue;
                    auto ot = e.value("old_text", e.value("old_string", std::string{}));
                    auto nt = e.value("new_text", e.value("new_string", std::string{}));
                    out.hunks.push_back({std::move(ot), std::move(nt)});
                }
                // WHILE STREAMING: window to the NEWEST hunk only — the
                // actual Write tail-window discipline this path always
                // claimed to mirror. Rendering every accumulated hunk let
                // the live card balloon past the viewport, committing its
                // top rows to native scrollback; the later Pending→Running
                // flip then RESTYLED every committed card row (connector
                // stripe bright_black→blue, status glyph ○→●) — a style-
                // only committed-row rewrite that trips maya's gate (the
                // scrollback_oracle write/edit turn caught it at t*-e-run).
                // One compact hunk keeps the live card inside the viewport
                // where lifecycle restyles are legal; the full indexed
                // multi-hunk render appears at settle (show_all below),
                // painted once, below the seam.
                if (streaming_now && out.hunks.size() > 1)
                    out.hunks.erase(out.hunks.begin(),
                                    out.hunks.end() - 1);
                // ...and keep that hunk inside the STREAMING BODY BUDGET:
                // tail-only, budget-derived lines per side. The budget is
                // load-bearing, not cosmetic: the event HEADER row sits
                // above the body, and everything below it (body + footer +
                // border + composer/status chrome ≈ 15 rows) must fit
                // under term_h so the header stays INSIDE the viewport
                // while the card streams. At 18-row terminals that leaves
                // 1 line per side (the config the oracle proves safe); at
                // taller terminals the preview widens up to the settled
                // profile's 6 lines per side — the user watches the hunk
                // stream in without risking the header crossing the seam.
                // Body rows = 1 stat chip + per_side × 2.
                if (streaming_now) {
                    const int per_side = std::clamp(
                        (stream_body_budget() - 1) / 2, 1, 6);
                    out.edit_head_per_side = 0;
                    out.edit_tail_per_side = per_side;
                }
                // Settled hunks render in FULL (show_all) in BOTH the
                // live tail and the frozen snapshot — byte-identical, so
                // the freeze handoff is a pure cache hit. Only STREAMING
                // stays elided (hunks grow line-by-line, would balloon
                // height every frame). The tall settled card is a
                // paint-once blit via its per-event hash_id, so a full
                // body is free per frame after the first paint.
                out.show_all = !streaming_now;
                return out;
            }
            auto ot = safe_arg(tc.args, "old_text");
            if (ot.empty()) ot = safe_arg(tc.args, "old_string");
            auto nt = safe_arg(tc.args, "new_text");
            if (nt.empty()) nt = safe_arg(tc.args, "new_string");
            if (!ot.empty() || !nt.empty()) {
                out.kind = Kind::EditDiff;
                // Full hunk in both live and frozen (settled); only
                // streaming stays elided. Live == frozen body.
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
            out.is_streaming = streaming_now;
            // Settled write renders the FULL body (show_all) in the live
            // tail AND the frozen snapshot — byte-identical, so the freeze
            // handoff keys both on the same hash_id and is a pure cache
            // hit (no committed-row shift = no duplicated/wiped card). The
            // user sees exactly what was written. The tall card is a
            // paint-once blit via its per-event hash_id (agent_timeline.
            // cpp), so the full body costs nothing per frame after the
            // first paint — no need to window the live card. Only
            // STREAMING stays windowed: the body is still growing and
            // would balloon height every frame.
            out.show_all = !streaming_now;
            if (streaming_now) {
                // Small tail slice → O(window) per frame and a bounded
                // card height. show_all=false makes maya render just its
                // `code_tail` lines from this slice; size that tail to
                // the streaming body budget (3 lines at 18-row terminals
                // — the oracle-proven floor — up to 12 on tall ones) so
                // the "watch it write" window uses the height available
                // without pushing the header row past the viewport top
                // (the committed-row-rewrite seam; see
                // stream_body_budget). Footer totals would be wrong on a
                // partial body, so suppress mid-stream (status bar
                // carries the live rate); it returns with the true total
                // the instant we settle.
                out.show_all = false;
                out.code_tail = std::clamp(stream_body_budget(), 3, 12);
                out.text = tail_window(content, kStreamTailLines);
                out.show_footer_stats = false;
            } else {
                // Terminal: the FULL body, live and frozen alike.
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
            out.text = std::string{body};
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
            out.text = std::string{body};
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
            out.text = std::string{body};
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
            out.text = std::string{tc.output()};
            out.text_color = text_tertiary;
        }
        return out;
    }

    // ── task (subagent): live activity feed while running, condensed
    //    report when settled. The feed is streamed into progress_text()
    //    via progress::emit (turns / tool calls / results / streamed
    //    text); the terminal output is the harvested "Subagent report".
    //    BashOutput gives the tail-oriented "watch it work" look while
    //    running; CodeBlock shows the full report once settled.
    if (n == "task") {
        // Hard 5-row ceiling on the card body: a subagent report can be
        // long, but the timeline card is a glance-able summary — the full
        // report is always in the transcript. Both the live feed and the
        // settled report are head-capped to kTaskBodyRows.
        constexpr std::size_t kTaskBodyRows = 5;
        if (tc.is_running()) {
            if (!tc.progress_text().empty()) {
                out.kind = Kind::BashOutput;
                // BashOutput is tail-oriented (newest activity at the
                // bottom) — keep the last few feed lines so the running
                // card shows the CURRENT step, not the stale header.
                out.text = tail_window(tc.progress_text(), kTaskBodyRows);
                out.text_color = text_tertiary;
                out.is_streaming = true;
            }
            return out;
        }
        if (tc.is_terminal() && !tc.output().empty()) {
            // Strip the redundant "Subagent report (type, N turns):"
            // header + the blank line after it — the card detail already
            // shows the type and turn count, so the body should lead with
            // the actual outcome line.
            std::string_view body = tc.output();
            if (auto nl = body.find('\n'); nl != std::string_view::npos
                && body.substr(0, nl).find("Subagent report") != std::string_view::npos) {
                body.remove_prefix(nl + 1);
                while (!body.empty() && (body.front() == '\n' || body.front() == ' '))
                    body.remove_prefix(1);
            }
            out.kind = Kind::CodeBlock;
            // keep 4 content lines + 1 "⋯ N more" marker = 5 rows max.
            out.text = head_window(body, kTaskBodyRows - 1);
            out.text_color = text_tertiary;
            // We pre-sliced to the ceiling; render it verbatim (no further
            // head+tail elision that would fight our marker).
            out.show_all = true;
        }
        return out;
    }

    // ── Failure fallback. Chrome flips to red so the body chrome
    //    matches the card's failure cue; body content stays dim.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = std::string{tc.output()};
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
