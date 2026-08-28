#pragma once
// Internal shared surface for the per-tool body-preview renderers.
//
// tool_body_preview_config() dispatches on the tool NAME to one
// `<tool>_body(...)` function per tool, each living in its own
// translation unit (edit_body.cpp, bash_body.cpp, …). This header is the
// contract between the dispatcher and those units: the shared parsing /
// windowing helpers they all reuse, plus one declaration per tool.
//
// NOT a public header — it lives next to the .cpp files and is included
// only by them and by the dispatcher. Keep it out of the public
// tool_body_preview.hpp so the tool set can grow without churning the
// widget adapter's public surface.

#include <cstddef>
#include <string>
#include <string_view>

#include <maya/widget/tool_body_preview.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"  // GrepHits

namespace agentty::ui::detail {

// ── Shared windowing / parsing helpers ────────────────────────────────

// During streaming the body grows by a delta every ~120ms and the view
// rebuilds the whole Turn every frame. Handing maya the FULL content
// means an O(N) copy into the Config plus maya walking the whole body to
// render the last few tail lines. Slice to a bounded tail window on our
// side so per-frame cost is O(window), not O(file). maya's FileWrite /
// CodeBlock / FileRead renderers are tail-anchored, so the visible output
// is byte-identical to feeding the full body. Keep a generous margin so
// the widget's tail budget is always satisfied.
inline constexpr std::size_t kStreamTailLines = 64;

// Streaming-card body budget. The event HEADER row must stay inside the
// viewport while a write/edit card streams; everything below it
// (body + footer + border + composer/status chrome ≈ 15 rows) must fit
// under term_h. Returns term_rows − 15, floored at 3 (the proven-safe
// minimum at 18-row terminals). Uses query_terminal_size (not
// available_height) because tool bodies are built at VIEW-BUILD time,
// before the render pass installs the sized RenderContext.
[[nodiscard]] int stream_body_budget();

// Keep the LAST keep_lines lines of s (tail-anchored window).
[[nodiscard]] std::string tail_window(std::string_view s,
                                      std::size_t keep_lines);

// Keep the FIRST keep_lines lines; if more follow, append a dim
// "⋯ N more lines" marker so the body has a hard row ceiling. Used by the
// subagent card — the head is the outcome line + key details, and the
// full report is always in the wire/transcript.
[[nodiscard]] std::string head_window(std::string_view s,
                                      std::size_t keep_lines);

// Best-effort path lookup: tools spell the field differently (write uses
// `file_path`, edit/read use `path`). Returns empty when nothing
// resembles a path.
[[nodiscard]] std::string read_path_arg(const nlohmann::json& args);

// Parse `## Matches in <path>` + `### L<start>-<end>` markers out of
// agentty's grep output and accumulate (path → {start lines}) into out.
void accumulate_grep_hits(const std::string& output, GrepHits& out);

// ── Per-tool body renderers ───────────────────────────────────────────
//
// Each fills `out` (kind + data) for the tools it owns and returns true;
// returns false to let the dispatcher fall through to the next candidate
// (the shared Failure fallback, or Kind::None). `out.chrome_color` is
// pre-set by the dispatcher to the tool's category color before the call.
//
// grep_hits is only consulted by read_body (FileRead highlight_lines);
// the others ignore it. A uniform signature keeps the dispatch table flat.
//
// The always-own renderers (edit/bash/write/task) return true
// unconditionally — they are NOT [[nodiscard]] since the dispatcher
// intentionally ignores the result. The fall-through renderers return
// false to defer to the shared Failure fallback, so THEIR result must be
// checked.

bool edit_body(const ToolUse& tc,
               maya::ToolBodyPreview::Config& out);
bool bash_body(const ToolUse& tc,
               maya::ToolBodyPreview::Config& out);
bool write_body(const ToolUse& tc,
                maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool git_diff_body(const ToolUse& tc,
                                 maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool read_body(const ToolUse& tc,
                             const GrepHits* grep_hits,
                             maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool web_fetch_body(const ToolUse& tc,
                                  maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool grep_body(const ToolUse& tc,
                             maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool diff_preview_body(const ToolUse& tc,
                                    maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool generic_list_body(const ToolUse& tc,
                                     maya::ToolBodyPreview::Config& out);
bool task_body(const ToolUse& tc,
               maya::ToolBodyPreview::Config& out);
[[nodiscard]] bool todo_body(const ToolUse& tc,
                             maya::ToolBodyPreview::Config& out);

} // namespace agentty::ui::detail
