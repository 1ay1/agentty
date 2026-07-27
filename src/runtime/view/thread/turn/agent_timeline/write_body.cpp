// tool_body_preview — `write` tool body.
//
// FileWrite: line-numbered body + lines/bytes footer. Settled writes
// render the FULL new-file content (show_all) so the user sees exactly
// what was written; streaming stays windowed to a small tail so a growing
// body never balloons the card height or fragments committed scrollback
// rows. See the seam discipline in the comments below.

#include "tool_body_common.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

bool write_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;

    // FAILED write: error body, same rationale as edit above.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = std::string{tc.output()};
        out.chrome_color = status_error;
        return true;
    }
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
    return true;
}

} // namespace agentty::ui::detail
