#include "moha/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"

#include <string>
#include <utility>
#include <vector>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "moha/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "moha/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"

namespace moha::ui {

maya::AgentTimeline::Config agent_timeline_config(const Message& msg,
                                                  int spinner_frame,
                                                  maya::Color rail_color) {
    int total = static_cast<int>(msg.tool_calls.size());
    int done  = 0;
    float total_elapsed = 0.0f;
    int running_idx = -1;

    std::vector<std::pair<std::string, int>> cat_counts;
    auto bump_cat = [&](const std::string& cat) {
        for (auto& [k, n] : cat_counts) if (k == cat) { ++n; return; }
        cat_counts.emplace_back(cat, 1);
    };

    for (std::size_t i = 0; i < msg.tool_calls.size(); ++i) {
        const auto& tc = msg.tool_calls[i];
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
    const GrepHits grep_hits = collect_grep_hits(msg);

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
    cfg.events.reserve(msg.tool_calls.size());
    for (const auto& tc : msg.tool_calls) {
        std::string detail = tool_timeline_detail(tc);
        if (detail.empty()) {
            detail = tc.is_running()  ? std::string{"running\xe2\x80\xa6"}
                   : tc.is_pending()  ? std::string{"queued\xe2\x80\xa6"}
                   : tc.is_approved() ? std::string{"approved\xe2\x80\xa6"}
                                      : std::string{"\xe2\x80\xa6"};
        }
        cfg.events.push_back({
            .name            = tool_display_name(tc.name.value),
            .detail          = std::move(detail),
            .elapsed_seconds = tc.is_terminal() ? tool_elapsed(tc) : 0.0f,
            .category_color  = tool_category_color(tc.name.value),
            .status          = tool_event_status(tc),
            .body            = tool_body_preview_config(tc, &grep_hits),
        });
    }

    // ── Footer: ✓ DONE / ✗ N FAILED / ⊘ N REJECTED, only when settled.
    if (done == total && total > 0) {
        int failed = 0, rejected = 0;
        for (const auto& tc : msg.tool_calls) {
            if (tc.is_failed())   ++failed;
            if (tc.is_rejected()) ++rejected;
        }
        maya::AgentTimelineFooter f;
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
        f.summary = std::to_string(total)
                  + (total == 1 ? " action   " : " actions   ")
                  + format_duration_compact(total_elapsed);
        cfg.footer = std::move(f);
    }

    // ── Title and border.
    std::string title = " " + small_caps("Actions") + "  \xc2\xb7  "
                      + std::to_string(done) + "/" + std::to_string(total);
    if (running_idx >= 0) {
        title += "  \xc2\xb7  " + tool_display_name(
            msg.tool_calls[static_cast<std::size_t>(running_idx)].name.value);
    } else if (done == total && total > 0) {
        title += "  \xc2\xb7  " + format_duration_compact(total_elapsed);
    }
    title += " ";

    bool all_done = (done == total && total > 0);
    cfg.title        = std::move(title);
    cfg.border_color = all_done ? muted : rail_color;
    return cfg;
}

} // namespace moha::ui
