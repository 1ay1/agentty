// tool_body_preview — `edit` tool body.
//
// Two rendering paths, picked by what data we have:
//   (a) FENCE-PARSE — for terminal-state edits, pull the diff payload out
//       of the ```diff … ``` block the edit tool writes into its output
//       text. Routes through Kind::GitDiff for interleaved −/+ coloring +
//       line anchors.
//   (b) ARGS-ECHO — streaming / pre-run, no diff exists yet: synthesize a
//       Kind::EditDiff from `tc.args.edits[*]`. The file's `before`
//       content isn't reachable from the view so this is the best preview
//       we can offer until execution lands.

#include "tool_body_common.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui::detail {

bool edit_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    const bool streaming_now = !tc.is_terminal();

    // FAILED edit: surface the error text as the body. Falling through to
    // the args-echo EditDiff rendered the *attempted* hunks and swallowed
    // the actual failure reason ("old_text not found", "file changed since
    // read") — the user saw a red ✗ with no explanation and the model's
    // self-correction hint was only on the wire, never on screen.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = std::string{tc.output()};
        out.chrome_color = status_error;
        return true;
    }
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
            // Settled edit ALWAYS renders the full diff (show_all) — in the
            // live tail AND the frozen snapshot, byte-identical. The user
            // reviews exactly what changed; the per-event hash_id
            // cell-cache (agent_timeline.cpp) makes the tall card a
            // paint-once blit even while it sits in the live tail, so a
            // full body costs nothing per frame after the first. Live ==
            // frozen body => the freeze handoff is a pure cache hit (no
            // committed-row shift).
            out.text       = std::string{body.substr(a, b - a)};
            out.show_all   = true;
            out.tail_only  = false;
            out.text_color = text_tertiary;
            return true;
        }
        // Fence missing — fall through to args-based EditDiff below.
    }

    if (tc.args.is_object()) {
        // Mirror Write's discipline: while the edit is streaming, the hunks
        // grow line-by-line (each `edits[i].new_text` delta arrives
        // mid-token), which would balloon the card height on every frame
        // and fragment any rows already pushed to native scrollback. Pin
        // the streaming preview to the tail window (show_all=false → maya's
        // tail_only renderer shows just the last N lines per hunk side);
        // expand to show_all only once the tool has settled and the body
        // is final.
        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            out.kind = Kind::EditDiff;
            // Full diff as soon as the edit is terminal — in the live tail
            // too, not only the frozen snapshot. While STREAMING keep the
            // elided per-side/per-hunk preview (hunks grow line-by-line,
            // would balloon height every frame); once settled the hunks are
            // final, so expanding immediately avoids the "stub then sudden
            // expand" lag and matches what freeze_range will build
            // (seamless handoff).
            out.is_streaming = streaming_now;
            out.hunks.reserve(it->size());
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                out.hunks.push_back({std::move(ot), std::move(nt)});
            }
            // WHILE STREAMING: maya's edit_diff_streaming renders one pinned
            // stat chip + a ROW-LEVEL live tail of the last
            // 2×edit_tail_per_side diff rows across ALL hunks. Feeding it
            // every hunk (no host-side windowing) is what keeps the tail
            // window FULL across hunk boundaries: a fresh hunk pops into the
            // args array mid-token with near-zero content, and the old
            // newest-hunk-only window collapsed the body to the bare chip on
            // that frame — the card visibly shrank upwards, then re-grew as
            // text arrived. With the cross-hunk row tail the height is
            // monotonically non-decreasing (content only ever grows), so the
            // card never shrinks mid-stream and the seam invariant below
            // still bounds its height.
            //
            // Tag the streaming stat chip with the ordinal of the hunk
            // currently landing ("edit 3 · −6 / +6") so the tail window
            // still conveys how many edits have already applied — zero extra
            // rows, and the ticking number stays inside the viewport by the
            // budget below.
            if (streaming_now)
                out.stream_hunk_no = static_cast<int>(it->size());
            // ...and keep the live card inside the STREAMING BODY BUDGET.
            // The budget is load-bearing, not cosmetic: the event HEADER row
            // sits above the body, and everything below it (body + footer +
            // border + composer/status chrome ≈ 15 rows) must fit under
            // term_h so the header stays INSIDE the viewport while the card
            // streams. Body rows = 1 stat chip + 2×per_side tail rows ≤
            // budget: at 18-row terminals that's chip + 2 rows (the config
            // the oracle proves safe); at taller terminals the tail widens
            // up to 12 rows — the user watches the newest rows stream past
            // without the header crossing the seam.
            if (streaming_now) {
                const int per_side = std::clamp(
                    (stream_body_budget() - 1) / 2, 1, 6);
                out.edit_head_per_side = 0;
                out.edit_tail_per_side = per_side;
            }
            // Settled hunks render in FULL (show_all) in BOTH the live tail
            // and the frozen snapshot — byte-identical, so the freeze
            // handoff is a pure cache hit. Only STREAMING stays elided
            // (hunks grow line-by-line, would balloon height every frame).
            // The tall settled card is a paint-once blit via its per-event
            // hash_id, so a full body is free per frame after the first
            // paint.
            out.show_all = !streaming_now;
            return true;
        }
        auto ot = safe_arg(tc.args, "old_text");
        if (ot.empty()) ot = safe_arg(tc.args, "old_string");
        auto nt = safe_arg(tc.args, "new_text");
        if (nt.empty()) nt = safe_arg(tc.args, "new_string");
        if (!ot.empty() || !nt.empty()) {
            out.kind = Kind::EditDiff;
            // Full hunk in both live and frozen (settled); only streaming
            // stays elided. Live == frozen body.
            out.show_all     = !streaming_now;
            out.is_streaming = streaming_now;
            // Same seam budget as the edits-array branch: streaming body =
            // chip + 2×per_side rows must fit under term_h so the event
            // header never crosses the viewport top.
            if (streaming_now) {
                const int per_side = std::clamp(
                    (stream_body_budget() - 1) / 2, 1, 6);
                out.edit_head_per_side = 0;
                out.edit_tail_per_side = per_side;
            }
            out.hunks.push_back({std::move(ot), std::move(nt)});
        }
    }
    return true;
}

} // namespace agentty::ui::detail
