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
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <maya/render/canvas.hpp>
#include <maya/render/diff.hpp>
#include <maya/render/renderer.hpp>
#include <maya/render/serialize.hpp>
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

// Side-channel: the most recent streaming_write_ms() view-build time,
// so main() can print it alongside the paint time without restructuring
// the function's return.
static double g_last_stream_view_ms = 0.0;

static std::string code_block(int n) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += "    auto x = compute(i) + offset; // line of plausible code\n";
    return out;
}

// Production-faithful render: maya's inline path (render_live) seeds a
// small canvas and GROWS it to fit the content (needed+8) via
// grow-and-retry — so the per-event hash_id cells always fit and blit.
// A probe that pins a fixed undersized canvas instead makes every tall
// card overflow `fits_rows`, so its cells never capture and every frame
// re-renders on the slow path: a pure HARNESS artifact (~80ms phantom
// cost) that production never pays. This helper reproduces the real
// grow-and-retry so the numbers reflect what the user actually feels.
static void render_grown(const maya::Element& root, maya::Canvas& canvas,
                         maya::StylePool& pool) {
    static thread_local std::vector<maya::layout::LayoutNode> lns;
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, lns, true);
    if (!lns.empty()) {
        const int needed = lns[0].computed.size.height.raw();
        if (needed > canvas.height()) {
            canvas.resize(canvas.width(), needed + 8);
            canvas.clear();
            maya::render_tree(root, canvas, pool, maya::theme::dark, lns, true);
        }
    }
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
    maya::Canvas canvas(120, 64, &pool);
    // Real loop: rebuild the tree AND repaint every frame (live tail
    // has no hash_id, so there's no cache to prime). Grow-and-retry so
    // the canvas fits the content exactly as production does — a fixed
    // undersized canvas would defeat the per-event cell cache and report
    // a phantom slow-path cost the live app never pays.
    render_grown(build_root(), canvas, pool);   // prime: grow + capture cells
    double best = 1e9;
    for (int i = 0; i < 7; ++i) {
        auto root = build_root();
        auto t0 = steady_clock::now();
        render_grown(root, canvas, pool);
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
    maya::Canvas canvas(120, 64, &pool);
    render_grown(build_root(), canvas, pool);   // prime: grow + capture cells
    double best = 1e9;
    for (int i = 0; i < 7; ++i) {
        auto root = build_root();
        auto t0 = steady_clock::now();
        render_grown(root, canvas, pool);
        best = std::min(best, ms(steady_clock::now() - t0));
    }
    return best;
}

// Measures the STREAMING-WRITE render cost: a write tool that is
// actively streaming (non-terminal, body growing) as the live tail.
// This is the "streaming mutating tool" scenario the user feels as
// slow. The live card is rebuilt + repainted every frame; we vary the
// streamed body size and report per-frame render cost. If this grows
// with body size, the streaming preview isn't bounding its work.
static double streaming_write_ms(int streamed_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"probe"};
    Message u; u.role = Role::User; u.text = "write a big file";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    ToolUse t;
    t.id   = ToolCallId{"sw_1"};
    t.name = ToolName{"write"};
    // Streaming state: the card body reads args["content"], which the
    // reducer fills incrementally from stream_decoded_value during the
    // stream. is_terminal is false → the streaming/tail-windowed preview.
    t.args = {{"file_path", "src/foo.cpp"},
              {"content", code_block(streamed_lines)}};
    t.args_streaming = "{\"file_path\":\"src/foo.cpp\",\"content\":\"";
    t.args_streaming += code_block(streamed_lines);   // approximate raw size
    t.status = ToolUse::Running{steady_clock::now(), {}};
    a.tool_calls.push_back(std::move(t));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // freeze the User
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
    double best = 1e9, best_view = 1e9;
    for (int i = 0; i < 9; ++i) {
        auto t_v0 = steady_clock::now();
        auto root = build_root();
        auto t_v1 = steady_clock::now();
        canvas.clear();
        auto t0 = steady_clock::now();
        maya::render_tree(root, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - t0));
        best_view = std::min(best_view, ms(t_v1 - t_v0));
    }
    g_last_stream_view_ms = best_view;
    return best;
}

// Measures the STREAMING-TEXT render cost: a single assistant message
// with NO tool calls whose markdown body grows token-by-token (the
// model writing a long prose answer). Under the agent_session
// discipline the WHOLE growing body stays live until settle — there is
// no mid-stream carve. Per-frame cost must stay flat anyway: maya's
// hash-keyed component cache + the StreamingMarkdown's committed-prefix
// reuse bound the per-frame work, which is exactly what this probes.
static double streaming_text_ms(int body_lines, double& view_out) {
    Model m;
    m.d.current.id = agentty::ThreadId{"probe"};
    Message u; u.role = Role::User; u.text = "explain this in detail";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    // Plausible long markdown answer: paragraphs + the occasional list.
    std::string body;
    for (int i = 0; i < body_lines; ++i) {
        if (i % 7 == 0) body += "## Section " + std::to_string(i) + "\n\n";
        body += "This is a sentence of a long streaming answer that the "
                "model is writing out token by token, line ";
        body += std::to_string(i);
        body += ".\n\n";
    }
    a.streaming_text = std::move(body);   // still streaming → not freezable
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);   // freeze the User
    // Replay the REAL per-tick cadence: the body doesn't appear all at
    // once — it grows ~1 KB/tick. No mid-stream freeze/trim (production
    // has none); the whole body accumulates live.
    {
        std::string full = std::move(m.d.current.messages.back().streaming_text);
        m.d.current.messages.back().streaming_text.clear();
        constexpr std::size_t kChunk = 1024;
        std::size_t fed = 0;
        while (fed < full.size()) {
            const std::size_t n = std::min(kChunk, full.size() - fed);
            m.d.current.messages.back().streaming_text.append(full, fed, n);
            fed += n;
        }
    }

    maya::StylePool pool;
    maya::Canvas canvas(120, 30000, &pool);
    double best = 1e9, best_view = 1e9;
    for (int i = 0; i < 9; ++i) {
        auto t_v0 = steady_clock::now();
        auto r = agentty::app::AgenttyApp::view(m);
        auto t_v1 = steady_clock::now();
        canvas.clear();
        maya::render_tree(r, canvas, pool, maya::theme::dark, true);
        best = std::min(best, ms(steady_clock::now() - t_v1));
        best_view = std::min(best_view, ms(t_v1 - t_v0));
    }
    view_out = best_view;
    return best;
}

// Measures WIRE BYTES per frame for a growing streaming-text answer —
// the metric that matters over SSH, where the bottleneck is bytes on
// the wire, not local CPU. Feeds the prose in 1 KB chunks (the real
// pacer cadence), runs the live freeze+trim each chunk, renders to a
// canvas, and diffs against the previous frame's canvas. Records the
// emitted ANSI byte count per frame. A bounded per-frame render means
// max_bytes/frame stays FLAT regardless of total body size; if a
// freeze handoff (mid-stream prefix freeze, or the end-of-turn
// freeze_through) shifts every live row, the diff re-emits the whole
// viewport and max_bytes spikes — that's the visible SSH lag/repaint.
struct WireCost { std::size_t max_frame; std::size_t total; int frames;
                  std::size_t max_steady; std::size_t max_boundary;
                  std::size_t end_of_turn; };

static WireCost streaming_text_wire_bytes(int body_lines) {
    Model m;
    m.d.current.id = agentty::ThreadId{"wire"};
    Message u; u.role = Role::User; u.text = "explain this in detail";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    std::string full;
    for (int i = 0; i < body_lines; ++i) {
        if (i % 7 == 0) full += "## Section " + std::to_string(i) + "\n\n";
        full += "This is a sentence of a long streaming answer that the "
                "model is writing out token by token, line ";
        full += std::to_string(i);
        full += ".\n\n";
    }
    // End the answer in a fenced code block whose closing ``` is the
    // LAST thing in the message (the common Claude "here's the code"
    // ending). find_block_boundary won't commit it during streaming
    // (no trailing newline after the close), so it renders via
    // render_tail until finish() — the divergence that drives the
    // "full repaint when the last block renders" symptom.
    full += "Here is the final snippet:\n\n";
    full += "```cpp\n";
    full += code_block(8);
    full += "```";

    constexpr int kW = 120, kH = 30000, kTermH = 40;
    maya::StylePool pool;
    // Two canvases: ping-pong front/back so each frame diffs against the
    // immediately-preceding rendered frame, exactly as the live loop does.
    maya::Canvas cv_a(kW, kH, &pool);
    maya::Canvas cv_b(kW, kH, &pool);
    maya::Canvas* prev = &cv_a;
    maya::Canvas* cur  = &cv_b;
    bool have_prev = false;

    auto render_into = [&](maya::Canvas& c) {
        auto r = agentty::app::AgenttyApp::view(m);
        c.clear();
        maya::render_tree(r, c, pool, maya::theme::dark, true);
    };

    WireCost wc{0, 0, 0, 0, 0, 0};
    constexpr std::size_t kChunk = 1024;
    std::size_t fed = 0;
    std::string out;
    std::size_t prev_frozen = m.ui.frozen.size();
    while (fed < full.size()) {
        const std::size_t n = std::min(kChunk, full.size() - fed);
        m.d.current.messages.back().streaming_text.append(full, fed, n);
        fed += n;
        // No mid-stream freeze/trim — production discipline.
        const bool freeze_happened = m.ui.frozen.size() != prev_frozen;
        prev_frozen = m.ui.frozen.size();

        render_into(*cur);
        if (have_prev) {
            out.clear();
            maya::diff(*prev, *cur, pool, out);
            wc.max_frame = std::max(wc.max_frame, out.size());
            wc.total += out.size();
            ++wc.frames;
            if (freeze_happened)
                wc.max_boundary = std::max(wc.max_boundary, out.size());
            else
                wc.max_steady = std::max(wc.max_steady, out.size());
        }
        std::swap(prev, cur);
        have_prev = true;
        (void)kTermH;
    }

    // End-of-turn settle: mirror finalize_turn's idle freeze. Drain the
    // last streaming_text into `text`, pre-settle the StreamingMarkdown
    // so the height is locked, then freeze_through the whole transcript
    // — the live tail becomes a frozen entry. Render the settled frame
    // and diff it against the last streaming frame. If the frozen render
    // is cell-identical to the pre-settle live render, this emits ~0
    // bytes; if it shifts even one row, the diff re-emits the viewport
    // — the visible end-of-turn repaint the user feels over SSH.
    //
    // CRITICAL: render the LAST STREAMING frame through the SAME
    // streaming widget (live tail path) with all bytes revealed, THEN
    // call finish() and render again. NOTE: with set_content fed the
    // FULL body at once, the eager-closing-fence commit (maya
    // boundary.cpp) already commits the trailing fence on the
    // set_content call, so this no longer reproduces the pre-fix
    // render_tail vs block-path divergence — the authoritative guard
    // for that is maya's `eager closing fence (no snap)` cell-equality
    // test. This residual ~2.4 KB is the spinner/cursor drop on settle,
    // which is irreducible and flat.
    {
        auto& last = m.d.current.messages.back();
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            last.text += last.streaming_text;
            last.streaming_text.clear();
        }
        if (last.role == Role::Assistant && !last.text.empty()) {
            auto& cache = m.ui.view_cache.message_md(m.d.current.id, last.id);
            if (!cache.streaming)
                cache.streaming = std::make_shared<maya::StreamingMarkdown>();
            // Live-tail render: full bytes revealed, but NOT finished —
            // the last block still goes through render_tail.
            cache.streaming->set_content(last.text);
            cache.streaming->set_live(true);
            cache.revealed_size    = last.text.size();
            cache.last_settled_size = static_cast<std::size_t>(-1);
        }
        render_into(*cur);            // last streaming frame (render_tail)
        std::swap(prev, cur);
        // Now settle: finish() commits the last block to the block path.
        if (last.role == Role::Assistant && !last.text.empty()) {
            auto& cache = m.ui.view_cache.message_md(m.d.current.id, last.id);
            cache.streaming->finish();
            cache.streaming->set_live(false);
            cache.last_settled_size = last.text.size();
        }
        m.s.phase = agentty::phase::Idle{};
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        render_into(*cur);            // settled frame (committed block path)
        out.clear();
        maya::diff(*prev, *cur, pool, out);
        wc.end_of_turn = out.size();
    }
    return wc;
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
        agentty::app::detail::clear_frozen(m);
        agentty::app::detail::freeze_through(m, 1);   // freeze the User

        // Production cadence: every settled sub-turn stays LIVE until
        // the turn finishes (agent_session discipline — no mid-run
        // freeze). This probe therefore measures the real live-tail
        // cost as the run accumulates edits; flat = win (maya's
        // per-event hash_id keeps settled tool cards cache-hits).
        static int c = 0;
        for (int e = 0; e < n; ++e) {
            Message a; a.role = Role::Assistant;
            ToolUse t;
            t.id   = ToolCallId{"e_" + std::to_string(++c)};
            t.name = ToolName{"edit"};
            nlohmann::json edits = nlohmann::json::array();
            const int bl = std::getenv("PROBE_BODY_LINES")
                             ? std::atoi(std::getenv("PROBE_BODY_LINES")) : 6;
            edits.push_back({{"old_text", code_block(bl)},
                             {"new_text", code_block(bl)}});
            t.args = {{"path", "src/foo.cpp"}, {"edits", edits}};
            auto now = steady_clock::now();
            std::string diff = "```diff\n";
            for (int l = 0; l < bl; ++l) diff += "-old line " + std::to_string(l) + "\n+new line " + std::to_string(l) + "\n";
            diff += "```";
            t.status = ToolUse::Done{now - milliseconds{5}, now, diff};
            a.tool_calls.push_back(std::move(t));
            m.d.current.messages.push_back(std::move(a));
        }
        Message tail; tail.role = Role::Assistant;
        tail.streaming_text = "more";
        m.d.current.messages.push_back(std::move(tail));
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

        // Count how many message-indices remain live (un-frozen). Under
        // the single-freeze discipline the whole active run is live, so
        // this GROWS with n — the probe verifies per-frame cost stays
        // flat anyway (hash_id cache hits on settled cards).
        const std::size_t live_msgs =
            m.d.current.messages.size() - m.ui.frozen_through;

        // Canvas sized to cover the FULL content height, as maya's
        // inline path does (the inline canvas always spans the whole
        // transcript). A short canvas would clip the live run's lower
        // events out of paint range, so their painted-cell cache never
        // populates and every frame re-renders them — a harness
        // artifact, not an app cost.
        const int canvas_h = std::getenv("PROBE_CANVAS_H")
                                ? std::atoi(std::getenv("PROBE_CANVAS_H")) : 40000;
        maya::StylePool pool;
        maya::Canvas canvas(120, canvas_h, &pool);
        double vbuild = 1e9, paint = 1e9;
        std::uint64_t miss_steady = 0;
        // Prime twice so maya's hash-keyed component cache captures the
        // frozen entries' cells (the warm/blit path the real loop hits).
        for (int w = 0; w < 2; ++w) {
            auto r = agentty::app::AgenttyApp::view(m);
            canvas.clear();
            maya::render_tree(r, canvas, pool, maya::theme::dark, true);
        }
        for (int it = 0; it < 9; ++it) {
            const std::uint64_t m0 =
                maya::render_detail::component_render_calls();
            auto t1 = steady_clock::now();
            auto r = agentty::app::AgenttyApp::view(m);
            auto t2 = steady_clock::now();
            canvas.clear();
            maya::render_tree(r, canvas, pool, maya::theme::dark, true);
            auto t3 = steady_clock::now();
            vbuild = std::min(vbuild, ms(t2 - t1));
            paint  = std::min(paint,  ms(t3 - t2));
            miss_steady = std::max(miss_steady,
                maya::render_detail::component_render_calls() - m0);
        }
        std::printf("%-8d | %12.3f | %12.3f | %12.3f | %5zu live | %6zu rows | miss=%llu\n",
                    n, vbuild, paint, vbuild + paint,
                    live_msgs, m.ui.frozen.row_total(),
                    (unsigned long long)miss_steady);
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
                    s.name, m.ui.frozen.row_total(), c.cold, c.warm, lt, ar);
    }

    scaling_breakdown();

    // ── Streaming write: a write tool actively streaming as the live
    // tail, body growing. This is the "streaming mutating tool" path
    // the user reports as slow. Per-frame render should be FLAT in the
    // streamed body size (the preview is tail-windowed); if it grows,
    // the streaming card isn't bounding its layout/paint work.
    std::printf("\nstreaming write (live card, growing body):\n");
    std::printf("%-14s | %12s | %12s\n", "streamed_lines", "view_ms", "render_ms");
    std::printf("---------------+--------------+--------------\n");
    for (int sl : {50, 200, 800, 3000, 8000}) {
        double r = streaming_write_ms(sl);
        std::printf("%-14d | %12.3f | %12.3f\n", sl, g_last_stream_view_ms, r);
    }

    // ── Streaming TEXT: a single assistant message, no tools, whose
    // markdown body grows as the model writes a long prose answer.
    // This is the uncovered gap — should grow with body size today.
    std::printf("\nstreaming text (live prose answer, growing body):\n");
    std::printf("%-12s | %12s | %12s\n", "body_lines", "view_ms", "render_ms");
    std::printf("-------------+--------------+--------------\n");
    for (int bl : {50, 200, 800, 2000, 5000}) {
        double vv = 0;
        double r = streaming_text_ms(bl, vv);
        std::printf("%-12d | %12.3f | %12.3f\n", bl, vv, r);
    }

    // ── Streaming TEXT wire bytes: bytes emitted to the terminal per
    // frame as the prose answer grows. This is the SSH-relevant metric
    // — CPU is flat (above), but if a freeze handoff re-emits the whole
    // viewport, max_bytes/frame spikes and the user on a slow link sees
    // lag + an end-of-turn repaint. Flat max_bytes = bounded wire cost.
    std::printf("\nstreaming text wire bytes (per-frame ANSI emitted):\n");
    std::printf("%-12s | %12s | %12s | %12s | %12s\n",
                "body_lines", "max_steady", "max_bndry", "mean/fr", "end_of_turn");
    std::printf("-------------+--------------+--------------+--------------+--------------\n");
    for (int bl : {50, 200, 800, 2000, 5000}) {
        auto wc = streaming_text_wire_bytes(bl);
        double mean = wc.frames ? double(wc.total) / wc.frames : 0.0;
        std::printf("%-12d | %12zu | %12zu | %12.0f | %12zu\n",
                    bl, wc.max_steady, wc.max_boundary, mean, wc.end_of_turn);
    }

    // ── Rehydrate footprint: rows the resume freeze seeds to the wire.
    // The 30s open on a real long thread was the WIRE EMIT of tens of
    // thousands of frozen rows (the terminal scrolling through them),
    // not CPU — load+rehydrate+cold render is ~200ms even on an 8MB
    // thread. rehydrate_frozen bounds the seeded tail to ~1500 rows
    // (the same window the live app re-renders); the rest stays on disk
    // and is recalled via the picker. Probe reports frozen rows/entries.
    std::printf("\nrehydrate footprint (bounded resume seed):\n");
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
                    rs.name, m.ui.frozen.row_total(), m.ui.frozen.size());
    }
    return 0;
}
