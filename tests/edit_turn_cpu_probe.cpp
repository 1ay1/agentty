// edit_turn_cpu_probe — measure the TRUE per-frame render cost of a turn
// that contains several SETTLED edit tools while ONE tool is still
// running.
//
// Why this shape: the AgentTimeline panel only gets a hash_id (→ maya
// panel-level cache-blit) once ALL tools are terminal. While any tool
// runs, the panel is re-BUILT and re-laid-out every frame. Each SETTLED
// edit event carries a per-event hash_id, so maya SHOULD cache-blit its
// body — but the panel wrapper, the connectors, the footer spinner, and
// the whole vstack layout still cost O(events)/frame, and if the edit
// bodies DON'T actually cache (tall card > canvas, or a key that jitters)
// the diff render of every edit body runs every frame. That is the CPU
// spike a user reports on "a turn with edit tools".
//
// This probe rebuilds the element tree every frame (like the real
// view() loop) and times render_tree — cold (first) vs warm (steady).
// warm≈cold ⇒ the edit bodies re-render every frame (the spike);
// warm≪cold ⇒ they cache-blit and the cost is just the live chrome.
//
// Run: ./build/edit_turn_cpu_probe
//      EDITS=8 EDIT_LINES=120 ./build/edit_turn_cpu_probe

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using Clock = std::chrono::steady_clock;
using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolUse;
using agentty::ToolCallId;
using agentty::ToolName;

static double ms(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d)
        .count();
}

static ToolUse settled_edit(int idx, int n_hunks, int lines_per_hunk) {
    ToolUse t;
    t.id   = ToolCallId{"edit_" + std::to_string(idx)};
    t.name = ToolName{"edit"};
    nlohmann::json edits = nlohmann::json::array();
    std::string diff = "```diff\n";
    for (int h = 0; h < n_hunks; ++h) {
        std::string ot, nt;
        for (int l = 0; l < lines_per_hunk; ++l) {
            ot += "    const auto old_" + std::to_string(l) + " = compute(l);\n";
            nt += "    const auto new_" + std::to_string(l) + " = compute(l) + 1;\n";
        }
        edits.push_back({{"old_text", ot}, {"new_text", nt}});
        diff += "@@ hunk " + std::to_string(h) + " @@\n";
        for (int l = 0; l < lines_per_hunk; ++l) {
            diff += "-    const auto old_" + std::to_string(l) + " = compute(l);\n";
            diff += "+    const auto new_" + std::to_string(l) + " = compute(l) + 1;\n";
        }
    }
    diff += "```";
    t.args = nlohmann::json{{"file_path", "src/module_" + std::to_string(idx) + ".cpp"},
                            {"edits", edits}};
    auto now = Clock::now();
    t.status = ToolUse::Done{now - std::chrono::milliseconds{10}, now, diff};
    return t;
}

static ToolUse running_bash() {
    ToolUse t;
    t.id   = ToolCallId{"bash_live"};
    t.name = ToolName{"bash"};
    t.args = nlohmann::json{{"command", "cmake --build build -j10"}};
    t.status = ToolUse::Running{Clock::now()};
    return t;
}

// A STREAMING edit: Running status, hunks present in args (no hash_id
// while non-terminal → the diff body re-renders every frame).
static ToolUse streaming_edit(int idx, int n_hunks, int lines_per_hunk) {
    ToolUse t = settled_edit(idx, n_hunks, lines_per_hunk);
    t.id   = ToolCallId{"sedit_" + std::to_string(idx)};
    t.status = ToolUse::Running{Clock::now()};
    return t;
}

static Model build_model(int edits, int e_lines, bool with_running) {
    Model m;
    m.d.current.id = agentty::ThreadId{"edit-probe"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};

    Message u; u.role = Role::User; u.text = "refactor these modules";
    m.d.current.messages.push_back(std::move(u));

    Message a; a.role = Role::Assistant;
    a.text = "Applying the refactor across the modules.";
    for (int i = 0; i < edits; ++i)
        a.tool_calls.push_back(settled_edit(i, /*n_hunks=*/2, e_lines));
    if (with_running) a.tool_calls.push_back(running_bash());
    m.d.current.messages.push_back(std::move(a));

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // freeze only the User
    if (with_running)
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    return m;
}

static double render_rebuild_ms(Model& m, int canvas_h, int iters) {
    auto build_root = [&] {
        return maya::AppLayout{{
            .thread        = agentty::ui::thread_config(m),
            .changes_strip = agentty::ui::changes_strip_config(m),
            .composer      = agentty::ui::composer_config(m),
            .status_bar    = agentty::ui::status_bar_config(m),
            .overlay       = std::nullopt,
        }}.build();
    };
    maya::StylePool pool;
    maya::Canvas canvas(120, canvas_h, &pool);
    // Warm the caches with a couple of untimed frames.
    for (int i = 0; i < 2; ++i) {
        auto root = build_root();
        canvas.clear();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
    }
    double best = 1e9, sum = 0;
    for (int i = 0; i < iters; ++i) {
        auto root = build_root();          // fresh tree each frame
        canvas.clear();
        auto ti = Clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        double v = ms(Clock::now() - ti);
        best = std::min(best, v);
        sum += v;
    }
    return sum / iters;   // mean steady-state ms/frame
}

int main() {
    const int edits   = std::getenv("EDITS")      ? std::atoi(std::getenv("EDITS"))      : 6;
    const int e_lines = std::getenv("EDIT_LINES") ? std::atoi(std::getenv("EDIT_LINES")) : 80;
    const int iters   = std::getenv("ITERS")      ? std::atoi(std::getenv("ITERS"))      : 200;

    std::printf("edit_turn_cpu_probe — %d settled edits x %d lines/hunk x 2 hunks\n\n",
                edits, e_lines);
    std::printf("%-22s | %-12s\n", "scenario", "mean ms/frame");
    std::printf("-----------------------+-------------\n");

    // Tall canvas so nothing overflows/clears (isolates the layout+paint
    // vs cache-blit question, not the canvas-clip cache-miss regime).
    const int canvas_h = 6000;

    // (A) all settled (panel has hash_id → whole-panel blit).
    {
        auto m = build_model(edits, e_lines, /*with_running=*/false);
        double r = render_rebuild_ms(m, canvas_h, iters);
        std::printf("%-22s | %10.3f\n", "all settled (cached)", r);
    }
    // (B) one running (panel NOT cached → rebuild; edit bodies SHOULD
    //     still per-event cache-blit). This is the user's scenario.
    {
        auto m = build_model(edits, e_lines, /*with_running=*/true);
        double r = render_rebuild_ms(m, canvas_h, iters);
        std::printf("%-22s | %10.3f  <- edit-tool turn while a tool runs\n",
                    "one running (live)", r);
    }
    // (C) the LAST edit is itself STREAMING (Running) — its diff body has
    //     no hash_id and re-renders every frame while the reveal ticks.
    {
        Model m;
        m.d.current.id = agentty::ThreadId{"edit-probe"};
        m.d.available_models.push_back({});
        m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
        Message u; u.role = Role::User; u.text = "refactor these modules";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        a.text = "Applying the refactor.";
        for (int i = 0; i < edits - 1; ++i)
            a.tool_calls.push_back(settled_edit(i, 2, e_lines));
        a.tool_calls.push_back(streaming_edit(edits, 2, e_lines));
        m.d.current.messages.push_back(std::move(a));
        agentty::app::detail::clear_frozen(m);
        agentty::app::detail::freeze_through(m, 1);
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        double r = render_rebuild_ms(m, canvas_h, iters);
        std::printf("%-22s | %10.3f  <- an edit itself is streaming\n",
                    "streaming edit body", r);
    }
    std::printf("\nat 30fps a 33ms budget is 100%% of one core; "
                "compare (B) to (A).\n");
    return 0;
}
