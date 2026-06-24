// tallcard_probe — isolate WHY a terminal (completed) big write card in
// the live tail re-renders every frame at ~120ms despite carrying a
// stable hash_id. Hypothesis: maya's ComponentElement cell-capture path
// (renderer.cpp) only stores cells when content_y + captured_rows <=
// canvas_h. A card TALLER than the canvas → cells cleared → measure-pass
// cache hit (gated on !cells.empty()) misses forever → full re-render.
//
// We build a live-tail model (User frozen, ONE terminal Assistant run
// with a write tool of N lines, stable hash_id since terminal + idle),
// then warm-render it on canvases of varying height and compare warm vs
// cold. warm≈cold ⇒ permanent cache miss; warm≈0 ⇒ cache blits.

#include <algorithm>
#include <chrono>
#include <cstdio>
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

using namespace std::chrono;
using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;

static double ms(steady_clock::duration d) {
    return duration_cast<duration<double, std::milli>>(d).count();
}

static std::string code_block(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "    auto x = compute(i) + offset; // line of plausible code\n";
    return out;
}

static ToolUse write_tool(int n_lines) {
    ToolUse t;
    static int c = 0;
    t.id   = ToolCallId{"call_" + std::to_string(++c)};
    t.name = ToolName{"write"};
    t.args = {{"file_path", "src/foo.cpp"}, {"content", code_block(n_lines)}};
    auto now = steady_clock::now();
    t.status = ToolUse::Done{now - milliseconds{10}, now,
                             "wrote " + std::to_string(n_lines) + " lines"};
    t.expanded = true;
    return t;
}

// A STREAMING write: status Running, content grown to n_lines. No
// hash_id is minted for non-terminal tools, so each frame re-lays-out
// the whole growing body from line 1 — the uncached cost the per-event
// cache can't touch. This is the case the reveal tick competes with.
static ToolUse streaming_write_tool(int n_lines) {
    ToolUse t;
    static int c = 0;
    t.id   = ToolCallId{"scall_" + std::to_string(++c)};
    t.name = ToolName{"write"};
    t.args = {{"file_path", "src/foo.cpp"}, {"content", code_block(n_lines)}};
    t.status = ToolUse::Running{steady_clock::now() - milliseconds{200}, {}};
    t.expanded = true;
    return t;
}

static Model build_streaming_model(int write_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"sprobe"};
    Message u; u.role = Role::User; u.text = "write the file";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    a.text = "Writing.";
    a.tool_calls.push_back(streaming_write_tool(write_lines));
    m.d.current.messages.push_back(std::move(a));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // freeze only the User
    return m;
}

// Live-tail model: frozen User + one or more terminal Assistant
// sub-turns each carrying a big write card, left UNFROZEN (live tail).
static Model build_live_tail_model(int n_subturns, int write_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"probe"};
    Message u; u.role = Role::User; u.text = "write the files";
    m.d.current.messages.push_back(std::move(u));
    for (int s = 0; s < n_subturns; ++s) {
        Message a; a.role = Role::Assistant;
        a.text = "Writing file " + std::to_string(s) + ".";
        a.tool_calls.push_back(write_tool(write_lines));
        m.d.current.messages.push_back(std::move(a));
    }
    // Session IDLE (default phase) so reserve_slot=false → the terminal
    // run gets a stable hash_id (the cacheable case we're probing).
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // freeze only the User
    (void)agentty::app::detail::trim_frozen_if_oversized(m);
    return m;
}

struct Cost { double cold; double warm; };

static Cost render_cost(Model& m, int canvas_h) {
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

    // WARM: SAME tree, prime then re-paint. If the hash_id cache stores
    // cells, iterations 2+ blit → warm≈0. If the card overflows the
    // canvas and cells are cleared, warm≈cold (permanent miss).
    auto root = build_root();
    canvas.clear();
    auto t0 = steady_clock::now();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);
    double cold = ms(steady_clock::now() - t0);

    double warm = 1e9;
    for (int i = 0; i < 9; ++i) {
        canvas.clear();
        auto ti = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        warm = std::min(warm, ms(steady_clock::now() - ti));
    }
    return {cold, warm};
}

// Production-faithful: REBUILD the element tree every frame (fresh
// ComponentElement instances), like the real view() loop does. Tests
// whether the hash-keyed cache survives per-frame tree rebuilds.
static double render_rebuild_ms(Model& m, int canvas_h) {
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
    double best = 1e9;
    for (int i = 0; i < 9; ++i) {
        auto root = build_root();          // fresh tree each frame
        canvas.clear();
        auto ti = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - ti));
    }
    return best;
}

int main() {
    std::printf("tallcard_probe — does a terminal live-tail big card cache?\n\n");
    std::printf("%-8s | %-9s | %-9s | %-9s | %-9s\n",
                "lines", "canvas_h", "cold_ms", "warm_ms", "cache?");
    std::printf("---------+-----------+-----------+-----------+----------\n");

    struct Case { int lines; int canvas_h; };
    const Case cases[] = {
        {300,  4000}, {300,  200},
        {3000, 4000}, {3000, 3200}, {3000, 200},
        {8000, 9000}, {8000, 200},
    };
    for (const auto& c : cases) {
        auto m = build_live_tail_model(1, c.lines);
        auto r = render_cost(m, c.canvas_h);
        const char* hit = (r.warm < r.cold * 0.25) ? "HIT" : "MISS";
        std::printf("%-8d | %-9d | %9.3f | %9.3f | %s\n",
                    c.lines, c.canvas_h, r.cold, r.warm, hit);
    }

    std::printf("\nmulti sub-turn live tail (canvas_h=4000, 3000-line cards each):\n");
    std::printf("%-12s | %-9s | %-9s\n", "n_subturns", "cold_ms", "warm_ms");
    std::printf("-------------+-----------+----------\n");
    for (int n : {1, 2, 3, 4}) {
        auto m = build_live_tail_model(n, 3000);
        auto r = render_cost(m, 4000 * n);   // canvas grows with content
        std::printf("%-12d | %9.3f | %9.3f\n", n, r.cold, r.warm);
    }
    std::printf("\nSTREAMING re-layout (Running tool, NO hash_id) — per-tick cost\n");
    std::printf("the reveal animation competes with, canvas grown to content:\n");
    std::printf("%-8s | %-12s\n", "lines", "per_tick_ms");
    std::printf("---------+--------------\n");
    for (int n : {100, 300, 1000, 3000}) {
        auto m = build_streaming_model(n);
        double r = render_rebuild_ms(m, std::max(200, n + 1000));
        std::printf("%-8d | %12.3f\n", n, r);
    }

    std::printf("\nREBUILD-each-frame (production view loop), 3000-line card:\n");
    std::printf("%-9s | %-14s\n", "canvas_h", "rebuild_ms");
    std::printf("----------+---------------\n");
    for (int ch : {4000, 3200, 200, 60}) {
        auto m = build_live_tail_model(1, 3000);
        double r = render_rebuild_ms(m, ch);
        std::printf("%-9d | %14.3f\n", ch, r);
    }

    return 0;
}
