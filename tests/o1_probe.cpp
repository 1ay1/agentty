// o1_probe — measure steady-state per-frame render cost AFTER the
// live-session freeze+trim flow (not the bounded rehydrate path the
// main bench exercises). Mirrors what a long thread actually hands
// maya every tick: full freeze_through over the whole thread, then
// trim_frozen_if_oversized, then render the resulting frozen tree.
//
// Prints warm render (the per-frame cost the user feels as lag/spinner
// stutter) and the final frozen row total, for a spread of thread
// shapes. The goal of the row-cap fix is that warm render stays flat
// (~O(1)) no matter how long / how tall the thread is.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/conversation.hpp"
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

static Model build(int n_turns, int write_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"probe"};
    for (int t = 0; t < n_turns; ++t) {
        Message u; u.role = Role::User; u.text = "do the thing please";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant; a.text = "On it.\n\nDone.";
        a.tool_calls.push_back(write_tool(write_lines));
        m.d.current.messages.push_back(std::move(a));
    }
    return m;
}

static double warm_render_ms(Model& m) {
    // Live-session flow: freeze the whole thread, then trim.
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    (void)agentty::app::detail::trim_frozen_if_oversized(m);

    auto root = maya::AppLayout{{
        .thread        = agentty::ui::thread_config(m),
        .changes_strip = agentty::ui::changes_strip_config(m),
        .composer      = agentty::ui::composer_config(m),
        .status_bar    = agentty::ui::status_bar_config(m),
        .overlay       = std::nullopt,
    }}.build();

    maya::StylePool pool;
    maya::Canvas canvas(120, 4000, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);  // cold
    double best = 1e9;
    for (int i = 0; i < 7; ++i) {
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - t0));
    }
    return best;
}

int main() {
    struct Shape { const char* name; int turns; int lines; };
    Shape shapes[] = {
        {"6t x 300-line",   6,   300},
        {"6t x 800-line",   6,   800},
        {"6t x 3000-line",  6,   3000},
        {"50t x 500-line",  50,  500},
        {"200t x 500-line", 200, 500},
        {"500t x 500-line", 500, 500},
    };
    std::printf("%-18s | %12s | %14s\n", "shape", "frozen_rows", "warm_render_ms");
    std::printf("-------------------+--------------+----------------\n");
    for (auto& s : shapes) {
        auto m = build(s.turns, s.lines);
        double w = warm_render_ms(m);
        std::printf("%-18s | %12zu | %14.2f\n",
                    s.name, m.ui.frozen_row_total, w);
    }
    return 0;
}
