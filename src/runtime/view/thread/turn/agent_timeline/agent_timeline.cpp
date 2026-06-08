#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"

#include <string>
#include <utility>
#include <vector>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

namespace agentty::ui {

maya::AgentTimeline::Config agent_timeline_config(std::span<const ToolUse> tool_calls,
                                                  int spinner_frame,
                                                  maya::Color rail_color) {
    int total = static_cast<int>(tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;

    std::vector<std::pair<std::string, int>> cat_counts;
    auto bump_cat = [&](const std::string& cat) {
        for (auto& [k, n] : cat_counts) if (k == cat) { ++n; return; }
        cat_counts.emplace_back(cat, 1);
    };

    for (std::size_t i = 0; i < tool_calls.size(); ++i) {
        const auto& tc = tool_calls[i];
        if (tc.is_terminal()) {
            ++done;
            total_elapsed += tool_elapsed(tc);
        }
        if (running_idx < 0 && (tc.is_running() || tc.is_approved()))
            running_idx = static_cast<int>(i);
        bump_cat(std::string{tool_category_label(tc.name.value)});
    }

    // Cross-tool semantics: scan completed Greps once up-front and build
    // a `path → {line numbers}` index. Subsequent Read/find_definition
    // tools that open any of those paths inherit the grep hits as
    // `highlight_lines`, anchoring the user's eye on lines the assistant
    // flagged earlier in the same turn instead of forcing a re-scan.
    // Mirrors agent_session.cpp's grep_hits → FileRead wiring in maya.
    const GrepHits grep_hits = collect_grep_hits(tool_calls);

    maya::AgentTimeline::Config cfg;
    cfg.frame = spinner_frame;

    // ── Stats. Pick a representative color per category so the badge
    //    matches the per-event tree glyph color downstream.
    for (const auto& [cat, n] : cat_counts) {
        maya::Color cc = (cat == "mutate")  ? accent
                       : (cat == "execute") ? success
                       : (cat == "plan")    ? warn
                       : (cat == "vcs")     ? highlight
                                            : info;
        cfg.stats.push_back({cat, n, cc});
    }

    // ── Events.
    cfg.events.reserve(tool_calls.size());
    for (const auto& tc : tool_calls) {
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty()) {
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};
        }
        // Per-event hash_id on every TERMINAL tool event — live tail
        // AND frozen snapshot. Once a tool is terminal its bytes are
        // immutable (status fixed, output().size() fixed), so the cache
        // key is content-addressed and stable across every subsequent
        // frame: maya blits the event's (header + body) sub-tree from
        // its component cache instead of re-laying-out ToolBodyPreview's
        // per-line row Elements. For a settled write/edit with hundreds
        // of lines that collapses hundreds of flex rows into one cached
        // blit — the dominant per-frame cost of a tall settled card
        // sitting in an in-flight assistant run.
        //
        // Why this is safe in the live tail (the historical concern):
        // the maya prev_cells desync that originally caused live cards
        // to collapse to their last line and lose the left border is
        // fixed in maya HEAD (card-border tests 5/5). The freeze handoff
        // is also a pure cache hit: freeze_range builds the same Element
        // sub-tree under the same hash_id, so maya's cache entry survives
        // the live→frozen transition with zero re-paint.
        //
        // Why this is needed: an in-flight assistant run can contain
        // many already-terminal write/edit/read cards (each sub-turn
        // settles before the next continuation arrives). Without a key,
        // every frame rebuilds and re-lays out every tall body for the
        // entire run — ~21 ms/frame for a 3000-line read, multiplied
        // by every settled card in the run. Over ssh that turns into
        // visible lag as the turn grows.
        //
        // Key: tool-call id + status + output size. Permanent cache hit
        // once terminal; running/pending events stay un-keyed (their
        // body still mutates each frame and would alias a stale blit).
        maya::CacheId event_hash_id;
        if (tc.is_terminal()) {
            event_hash_id = maya::CacheIdBuilder{}
                .add(std::string_view{"agentty.tool_event"})
                .add(std::string_view{tc.id.value})
                .add(static_cast<std::uint64_t>(tc.status.index()))
                .add(static_cast<std::uint64_t>(tc.output().size()))
                .build();
        }

        cfg.events.push_back({
            .name            = tool_display_name(tc.name.value),
            .detail          = std::move(detail),
            // Live elapsed for running/pending too — keeps the row's
            // right-edge duration cell present from the moment the
            // event renders, so the row doesn't horizontally snap
            // when the tool flips to terminal. tool_elapsed() uses
            // steady_clock::now() when finished_at is unset, which
            // is exactly the live counter we want.
            .elapsed_seconds = tool_elapsed(tc),
            .category_color  = tool_category_color(tc.name.value),
            .status          = tool_event_status(tc),
            .body            = tool_body_preview_config(tc, &grep_hits),
            .hash_id         = event_hash_id,
        });
    }

    // ── Footer. Present for the entire lifetime of the panel (live and
    //    settled) so the panel's row count doesn't grow by 1 when the
    //    last tool transitions to done. Height stability across that
    //    transition is what keeps panels straddling the scrollback seam
    //    from leaving rail / border fragments stranded: maya's row diff
    //    handles in-place row mutations fine, but a height delta on a
    //    panel that's already partially in native scrollback can't
    //    rewrite the rows that scrolled off.
    if (total > 0) {
        int failed = 0, rejected = 0;
        for (const auto& tc : tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        const bool all_done = (done == total);
        maya::AgentTimelineFooter f;
        if (all_done) {
            f.glyph = "\xe2\x9c\x93";   // ✓
            f.text  = "done";
            f.color = success;
            if (failed > 0) {
                f.glyph = "\xe2\x9c\x97";           // ✗
                f.text  = std::to_string(failed) + " failed";
                f.color = danger;
            } else if (rejected > 0) {
                f.glyph = "\xe2\x8a\x98";           // ⊘
                f.text  = std::to_string(rejected) + " rejected";
                f.color = warn;
            }
        } else {
            f.glyph = "\xe2\x97\x8f";   // ●
            f.text  = "running";
            f.color = muted;
        }
        f.summary = std::to_string(done) + "/" + std::to_string(total)
                  + (total == 1 ? " action   " : " actions   ")
                  + format_duration_compact(total_elapsed);
        cfg.footer = std::move(f);
    }

    // ── Title and border. Left side: "ACTIONS · done/total". Right side
    //    (border-text-end): currently-running tool name while in flight,
    //    or the total elapsed once settled — splitting the two pins the
    //    elapsed to the right edge instead of leaving it left-glued to
    //    the action count.
    std::string title = " " + small_caps("Actions") + "  \xc2\xb7  "
                      + std::to_string(done) + "/" + std::to_string(total) + " ";
    std::string title_end;
    if (running_idx >= 0) {
        title_end = " " + tool_display_name(
            tool_calls[static_cast<std::size_t>(running_idx)].name.value) + " ";
    } else if (done == total && total > 0) {
        title_end = " " + format_duration_compact(total_elapsed) + " ";
    }

    bool all_done = (done == total && total > 0);
    cfg.title        = std::move(title);
    cfg.title_end    = std::move(title_end);
    cfg.border_color = all_done ? muted : rail_color;
    return cfg;
}

} // namespace agentty::ui
