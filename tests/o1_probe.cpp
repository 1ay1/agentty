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

#include "agentty/runtime/app/program.hpp"
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

struct RenderCost { double cold; double warm; };

// Measures BOTH the cold-paint cost (fresh tree every iteration —
// every ComponentElement is a cache miss, the pessimal case the old
// probe reported) and the cache-hit warm cost (rebuild the SAME tree
// each frame so maya's hash_id-keyed ComponentCache blits the settled
// entries instead of repainting them — the cost the user ACTUALLY
// feels per frame in the live run loop, where frozen entries don't
// change between ticks).
static RenderCost warm_render_ms(Model& m) {
    // Live-session flow: freeze the whole thread, then trim.
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    (void)agentty::app::detail::trim_frozen_if_oversized(m);

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
    maya::Canvas canvas(120, 4000, &pool);

    // COLD: fresh tree each iteration (cache miss every entry).
    double cold = 1e9;
    for (int i = 0; i < 7; ++i) {
        auto root = build_root();
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        cold = std::min(cold, ms(steady_clock::now() - t0));
    }

    // WARM (cache-hit): one tree, painted repeatedly. After the first
    // paint populates the hash_id cache, subsequent paints blit the
    // settled cells — this is the real per-frame loop cost.
    auto root = build_root();
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);  // prime cache
    double warm = 1e9;
    for (int i = 0; i < 7; ++i) {
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        warm = std::min(warm, ms(steady_clock::now() - t0));
    }
    return {cold, warm};
}

// Measures the LIVE-TAIL render cost: the most-recent turn is NOT
// frozen — it sits in cfg.live_tail, which carries no hash_id and is
// REBUILT + REPAINTED from scratch on every frame. This is what the
// user feels WHILE a write/edit tool is active (streaming or just
// settled-but-not-yet-frozen): the in-flight turn's whole body —
// including a 3000-line write card — is re-laid-out and re-painted
// 30-60x/sec. freeze_through stops at the last user message so the
// final assistant run stays live, exactly as during an active run.
static double live_tail_ms(Model& m) {
    agentty::app::detail::clear_frozen(m);
    // Freeze everything EXCEPT the final assistant run: find the last
    // User message and freeze up to it, leaving the trailing turn live.
    const auto& msgs = m.d.current.messages;
    std::size_t live_start = msgs.size();
    for (std::size_t k = msgs.size(); k-- > 0; ) {
        if (msgs[k].role == Role::User) { live_start = k; break; }
    }
    agentty::app::detail::freeze_through(m, live_start);
    (void)agentty::app::detail::trim_frozen_if_oversized(m);

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
    maya::Canvas canvas(120, 4000, &pool);
    // Real loop: rebuild the tree AND repaint every frame (live tail
    // has no hash_id, so there's no cache to prime).
    double best = 1e9;
    for (int i = 0; i < 7; ++i) {
        auto root = build_root();
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - t0));
    }
    return best;
}

// Measures the ACTIVE-RUN render cost — the real symptom. Simulates an
// in-flight auto-pilot run: the session is active (spinner ticking),
// and the final assistant run has an EARLIER sub-turn carrying a
// completed big write/edit card plus a streaming tail sub-turn. The
// whole run is non-terminal (the tail streams), so freeze_through
// can't freeze it — it stays in the live tail and repaints every
// frame. This is what the user feels "while edit/write tools are
// used": the big card from a prior sub-turn re-laid-out 30-60x/sec
// for the rest of the run.
static double active_run_ms(int write_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"probe"};
    // A couple of settled exchanges first.
    for (int t = 0; t < 3; ++t) {
        Message u; u.role = Role::User; u.text = "do the thing please";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant; a.text = "On it.";
        a.tool_calls.push_back(write_tool(write_lines));
        m.d.current.messages.push_back(std::move(a));
    }
    // In-flight run: User + assistant sub-turn with a DONE big write +
    // a streaming sub-turn (the tail) that keeps the run non-terminal.
    {
        Message u; u.role = Role::User; u.text = "now do more";
        m.d.current.messages.push_back(std::move(u));
        Message a1; a1.role = Role::Assistant; a1.text = "Working on it.";
        a1.tool_calls.push_back(write_tool(write_lines));   // DONE, settled
        m.d.current.messages.push_back(std::move(a1));
        Message a2; a2.role = Role::Assistant;              // streaming tail
        a2.streaming_text = "Let me also";
        m.d.current.messages.push_back(std::move(a2));
    }
    // Mark the session active so reserve_slot / spinner engage exactly
    // as in the real run loop.
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, m.d.current.messages.size());
    (void)agentty::app::detail::trim_frozen_if_oversized(m);

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
    maya::Canvas canvas(120, 4000, &pool);
    double best = 1e9;
    for (int i = 0; i < 7; ++i) {
        auto root = build_root();
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - t0));
    }
    return best;
}

// SCALING breakdown: an in-flight run accumulating N settled edits,
// one per sub-turn (the real auto-pilot shape), plus a streaming tail
// keeping the run non-terminal. Measures view-build (turn_config) and
// paint SEPARATELY so we can see WHICH grows with N. Flat = win.
static void scaling_breakdown() {
    std::printf("\nin-flight run, N accumulated edits — view vs paint:\n");
    std::printf("%-8s | %12s | %12s | %12s\n",
                "n", "view_ms", "paint_ms", "total_ms");
    std::printf("---------+--------------+--------------+--------------\n");
    for (int n : {1, 5, 10, 20, 40, 80, 160, 320}) {
        Model m;
        m.d.current.id = agentty::ThreadId{"scale"};
        Message u; u.role = Role::User; u.text = "do many edits";
        m.d.current.messages.push_back(std::move(u));
        for (int e = 0; e < n; ++e) {
            Message a; a.role = Role::Assistant;
            ToolUse t;
            static int c = 0;
            t.id   = ToolCallId{"e_" + std::to_string(++c)};
            t.name = ToolName{"edit"};
            nlohmann::json edits = nlohmann::json::array();
            edits.push_back({{"old_text", code_block(6)},
                             {"new_text", code_block(6)}});
            t.args = {{"path", "src/foo.cpp"}, {"edits", edits}};
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now, "edited"};
            a.tool_calls.push_back(std::move(t));
            m.d.current.messages.push_back(std::move(a));
        }
        Message tail; tail.role = Role::Assistant;
        tail.streaming_text = "more";
        m.d.current.messages.push_back(std::move(tail));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

        agentty::app::detail::clear_frozen(m);
        // Real auto-pilot flow: the User turn is frozen on submit, then
        // each settled sub-turn freezes its completed prefix
        // incrementally (freeze_settled_subturns runs on every
        // ToolExecOutput). Replay both — calling the mid-run freeze
        // once per sub-turn so frozen accrues as separate entries
        // (exactly the real per-settle cadence), which lets trim bound
        // the row total. Measures the LIVE frame the user sees.
        agentty::app::detail::freeze_through(m, 1);   // freeze the User
        agentty::app::detail::freeze_settled_subturns(m);
        (void)agentty::app::detail::trim_frozen_if_oversized(m);

        // Count how many message-indices remain live (un-frozen). The
        // whole point of the mid-run freeze: this stays ~constant (1-2)
        // no matter how many edits accumulated.
        const std::size_t live_msgs =
            m.d.current.messages.size() - m.ui.frozen_through;

        // Canvas sized to the trimmed content (+chrome), as maya's
        // inline path allocates — NOT a fixed 60000. This is what the
        // user's terminal actually pays per frame.
        const int canvas_h = static_cast<int>(
            m.ui.frozen_row_total) + 200;
        maya::StylePool pool;
        maya::Canvas canvas(120, std::max(500, canvas_h), &pool);
        double vbuild = 1e9, paint = 1e9;
        // Prime twice so maya's hash-keyed component cache captures the
        // frozen entries' cells (the warm/blit path the real loop hits).
        for (int w = 0; w < 2; ++w) {
            auto r = agentty::app::AgenttyApp::view(m);
            canvas.clear();
            maya::render_tree(r, canvas, pool, maya::theme::dark, true);
        }
        for (int it = 0; it < 9; ++it) {
            auto t1 = steady_clock::now();
            auto r = agentty::app::AgenttyApp::view(m);
            auto t2 = steady_clock::now();
            canvas.clear();
            maya::render_tree(r, canvas, pool, maya::theme::dark, true);
            auto t3 = steady_clock::now();
            vbuild = std::min(vbuild, ms(t2 - t1));
            paint  = std::min(paint,  ms(t3 - t2));
        }
        std::printf("%-8d | %12.3f | %12.3f | %12.3f | %5zu live | %6zu rows\n",
                    n, vbuild, paint, vbuild + paint,
                    live_msgs, m.ui.frozen_row_total);
    }
}

int main() {
    struct Shape { const char* name; int turns; int lines; };
    Shape shapes[] = {
        {"6t x 300-line",   6,   300},
        {"6t x 800-line",   6,   800},
        {"6t x 3000-line",  6,   3000},
        {"3t x 3000-line",  3,   3000},
        {"10t x 2000-line", 10,  2000},
        {"50t x 500-line",  50,  500},
        {"200t x 500-line", 200, 500},
        {"500t x 500-line", 500, 500},
    };
    std::printf("%-18s | %12s | %10s | %10s | %12s | %12s\n",
                "shape", "frozen_rows", "cold_ms", "warm_ms", "livetail_ms", "activerun_ms");
    std::printf("-------------------+--------------+------------+------------+--------------+--------------\n");
    for (auto& s : shapes) {
        auto m  = build(s.turns, s.lines);
        auto c  = warm_render_ms(m);
        auto m2 = build(s.turns, s.lines);
        double lt = live_tail_ms(m2);
        double ar = active_run_ms(s.lines);
        std::printf("%-18s | %12zu | %10.2f | %10.2f | %12.2f | %12.2f\n",
                    s.name, m.ui.frozen_row_total, c.cold, c.warm, lt, ar);
    }

    scaling_breakdown();

    // ── Rehydrate budget: what an old thread paints on resume.
    // The visible "rendering from top" lag on thread open is the wire
    // emit of the rehydrated frozen rows. rehydrate_frozen should bound
    // this to ~one viewport. Probe: a long thread ending in a big
    // auto-pilot run, report the frozen rows the FIRST resume frame
    // emits. (term height here is whatever the test env reports.)
    std::printf("\nrehydrate budget (resume paint height):\n");
    std::printf("%-26s | %12s | %12s\n",
                "thread shape", "frozen_rows", "frozen_ent");
    std::printf("---------------------------+--------------+--------------\n");
    struct RShape { const char* name; int prior_turns; int final_edits; };
    for (auto rs : {RShape{"50t + 5-edit final", 50, 5},
                    RShape{"50t + 40-edit final", 50, 40},
                    RShape{"200t + 80-edit final", 200, 80}}) {
        Model m;
        m.d.current.id = agentty::ThreadId{"rehy"};
        for (int t = 0; t < rs.prior_turns; ++t) {
            Message u; u.role = Role::User; u.text = "go";
            m.d.current.messages.push_back(std::move(u));
            Message a; a.role = Role::Assistant; a.text = "On it.";
            a.tool_calls.push_back(write_tool(40));
            m.d.current.messages.push_back(std::move(a));
        }
        // One big final auto-pilot run: many edit sub-turns.
        Message uf; uf.role = Role::User; uf.text = "do many edits";
        m.d.current.messages.push_back(std::move(uf));
        for (int e = 0; e < rs.final_edits; ++e) {
            Message a; a.role = Role::Assistant;
            ToolUse t;
            static int c = 0;
            t.id = ToolCallId{"re_" + std::to_string(++c)};
            t.name = ToolName{"edit"};
            nlohmann::json edits = nlohmann::json::array();
            edits.push_back({{"old_text", code_block(6)},
                             {"new_text", code_block(6)}});
            t.args = {{"path", "src/foo.cpp"}, {"edits", edits}};
            auto now = steady_clock::now();
            t.status = ToolUse::Done{now - milliseconds{5}, now, "edited"};
            a.tool_calls.push_back(std::move(t));
            m.d.current.messages.push_back(std::move(a));
        }
        agentty::app::detail::rehydrate_frozen(m);
        std::printf("%-26s | %12zu | %12zu\n",
                    rs.name, m.ui.frozen_row_total, m.ui.frozen.size());
    }
    return 0;
}
