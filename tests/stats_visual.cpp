// stats_visual — dump every stats tab to the terminal, in colour.
//
// The unit tests assert on column POSITIONS and on numbers; neither can
// tell you whether the panel is pleasant to look at. This is the tool for
// that: it builds a realistic thread, drives the real view function, and
// emits real ANSI so `./build/dev/stats_visual` shows exactly what a user
// would see — bars, bands, braille plots and all.
//
// Not a ctest entry (NO_TEST): its output is for a human, and a test that
// asserts "this looks nice" asserts nothing.
//
//   ./build/stats_visual              every tab, 76 cols
//   ./build/stats_visual 100          every tab, 100 cols
//   ./build/stats_visual 76 Cache     one tab
//   ./build/stats_visual 76 Tools 8   one tab, scrolled 8 rows
//
// A tab taller than the panel's viewport SCROLLS in the real app, which a
// one-shot dump cannot do — so this tool sets a tall viewport and prints
// the whole tab at once. That is the point: you are inspecting layout,
// and a figure you have to scroll to is a figure you cannot review.
//
// The thread it builds is deliberately messy — a failed tool call, a
// retried turn, a cache-warm session, uneven token counts — because a
// stats panel that only looks good on tidy data is a stats panel that
// looks bad in production.

#include "agentty/domain/stats/tabs.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/stats.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

Message user_turn(const char* text) {
    Message m;
    m.role = Role::User;
    m.text = text;
    return m;
}

// One assistant turn with everything a real turn carries.
Message turn(const char* model, smart::ModelRole role,
             std::uint32_t in, std::uint32_t out,
             std::uint32_t cache_r = 0, std::uint32_t cache_w = 0,
             std::uint32_t ttft = 420, std::uint32_t stream_ms = 3800,
             std::uint32_t reasoning = 0, std::uint16_t retries = 0) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    m.served_role  = role;
    Message::Telemetry t;
    t.input_tokens     = in;
    t.output_tokens    = out;
    t.reasoning_tokens = reasoning;
    t.cache_read       = cache_r;
    t.cache_creation   = cache_w;
    t.ttft_ms          = ttft;
    t.stream_ms        = stream_ms;
    t.wire_bytes       = out * 4;
    t.transient_retries = retries;
    m.telemetry = t;
    if (reasoning) {
        m.reasoning_ms = 2400;
        m.thinking_blocks.push_back({"…", "sig", ""});
    }
    return m;
}

void add_tool(Message& m, const char* name, int ms, bool ok = true) {
    ToolUse t;
    t.id   = ToolCallId{std::string{name} + std::to_string(ms)};
    t.name = ToolName{name};
    const auto start = std::chrono::steady_clock::now();
    if (ok) {
        ToolUse::Done d;
        d.started_at  = start;
        d.finished_at = start + std::chrono::milliseconds{ms};
        t.status = d;
    } else {
        ToolUse::Failed f;
        f.started_at  = start;
        f.finished_at = start + std::chrono::milliseconds{ms};
        t.status = f;
    }
    m.tool_calls.push_back(std::move(t));
}

Model realistic_thread() {
    Model m;
    m.d.current.id = ThreadId{"visual"};
    using R = smart::ModelRole;

    auto push = [&](const char* prompt, Message a) {
        m.d.current.messages.push_back(user_turn(prompt));
        m.d.current.messages.push_back(std::move(a));
    };

    // A cold first turn: nothing cached, the model thinks hard.
    {
        auto a = turn("claude-opus-4-5", R::Strategic, 8200, 1400, 0, 8200,
                      1200, 9400, 900);
        add_tool(a, "read", 42);
        add_tool(a, "grep", 88);
        push("plan the refactor", std::move(a));
    }
    // Warm turns: the prefix is now cached, so input drops and TTFT halves.
    {
        auto a = turn("claude-sonnet-4-5", R::Implementation, 900, 2100,
                      9600, 0, 260, 6100);
        add_tool(a, "read", 31);
        add_tool(a, "edit", 120);
        add_tool(a, "edit", 96);
        push("apply it to the first file", std::move(a));
    }
    {
        auto a = turn("claude-sonnet-4-5", R::Implementation, 1100, 1800,
                      11800, 0, 240, 5200);
        add_tool(a, "edit", 140);
        add_tool(a, "bash", 2400);          // a slow one, for the p95
        push("and the second", std::move(a));
    }
    // A cheap utility turn.
    {
        auto a = turn("claude-haiku-4-5", R::Utility, 400, 320, 12400, 0,
                      140, 900);
        add_tool(a, "grep", 24);
        push("what changed?", std::move(a));
    }
    // A turn that had trouble: two retries and a failed tool call.
    {
        auto a = turn("claude-sonnet-4-5", R::Implementation, 1300, 2600,
                      13100, 0, 3100, 11200, 0, 2);
        a.telemetry->mid_stream_failures = 1;
        add_tool(a, "bash", 5200, /*ok=*/false);
        add_tool(a, "read", 38);
        push("run the tests", std::move(a));
    }
    // A big generation, for the plot's peak.
    {
        auto a = turn("claude-opus-4-5", R::Strategic, 2200, 5400, 14200, 0,
                      680, 21400, 1800);
        push("write the design doc", std::move(a));
    }
    {
        auto a = turn("claude-haiku-4-5", R::Utility, 500, 260, 15900, 0,
                      120, 700);
        push("commit it", std::move(a));
    }

    // A proactive-retrieval injection on the last question.
    auto& q = m.d.current.messages.emplace_back(user_turn("why is it slow?"));
    Message::ProactiveContext pc;
    pc.confidence = 0.82;
    q.proactive = pc;
    m.d.current.messages.push_back(
        turn("claude-sonnet-4-5", R::Implementation, 1000, 1600, 16400, 0,
             300, 4400));

    // A long tail of tool calls, shaped like a real coding session: a
    // dominant shell/edit pair, a handful of reads, and several tools
    // used once or twice. This is what exercises the ring's "other"
    // bucket and its categorical palette — a tidy 4-tool thread does not.
    {
        auto a = turn("claude-sonnet-4-5", R::Implementation, 1200, 1900,
                      15100, 0, 280, 5600);
        for (int i = 0; i < 22; ++i) add_tool(a, "shell", 40 + i * 7);
        for (int i = 0; i < 9;  ++i) add_tool(a, "edit",  90 + i * 11);
        for (int i = 0; i < 4;  ++i) add_tool(a, "read",  25 + i * 5);
        add_tool(a, "write", 130);
        add_tool(a, "git_commit", 260);
        add_tool(a, "list_dir", 18);
        add_tool(a, "shell", 4400, /*ok=*/false);
        push("land the whole change", std::move(a));
    }

    return m;
}

// Render one panel and print it with real SGR.
void dump(const Model& m, int w, int scroll) {
    maya::StylePool pool;
    maya::Canvas canvas(w, 60, &pool);
    // TWICE, with the offset re-applied in between. maya's ScrollState
    // clamps y against max_y, and max_y is only written back AFTER a
    // layout pass — so on a cold state the first render clamps y to 0 and
    // the offset is silently lost. The real app never notices (frame 2
    // has the measurement from frame 1), but a one-shot dump has to run
    // the pass, restore what it asked for, and then paint.
    maya::render_tree(ui::stats_panel(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);
    // ONE_PASS=1 stops here: a diagnostic for telling a genuine layout bug
    // apart from an artifact of this tool's own double render.
    if (std::getenv("ONE_PASS")) return;
    if (auto* o = m.ui.panel.get<pn::Stats>()) {
        o->scroll.y = scroll;
        o->scroll.clamp();
    }
    maya::render_tree(ui::stats_panel(m), canvas, pool, maya::theme::dark,
                      /*auto_height=*/true);
    if (std::getenv("STATS_VISUAL_DEBUG")) {
        const auto el = ui::stats_panel(m);
        std::fprintf(stderr, "[scroll y=%d max_y=%d  measured(1<<14)=%d measured(w)=%d]\n",
                     m.ui.panel.get<pn::Stats>()->scroll.y,
                     m.ui.panel.get<pn::Stats>()->scroll.max_y,
                     maya::measure_element(el, 1 << 14).height.value,
                     maya::measure_element(el, w).height.value);
    }

    int last = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < w; ++x)
            if (canvas.get(x, y).character != 0
                && canvas.get(x, y).character != U' ') { last = y; break; }

    for (int y = 0; y <= last; ++y) {
        std::string line;
        std::uint16_t cur = 0xFFFF;
        for (int x = 0; x < w; ++x) {
            const auto cell = canvas.get(x, y);
            if (cell.style_id != cur) {
                line += maya::Style::reset_sgr();
                line += pool.get(cell.style_id).to_sgr();
                cur = cell.style_id;
            }
            const char32_t ch = cell.character ? cell.character : U' ';
            if (ch < 0x80) {
                line += static_cast<char>(ch);
            } else if (ch < 0x800) {
                line += static_cast<char>(0xC0 | (ch >> 6));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else if (ch < 0x10000) {
                line += static_cast<char>(0xE0 | (ch >> 12));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                line += static_cast<char>(0xF0 | (ch >> 18));
                line += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
                line += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        line += maya::Style::reset_sgr();
        std::printf("%s\n", line.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int w = argc > 1 ? std::atoi(argv[1]) : 76;
    const std::string_view only = argc > 2 ? argv[2] : "";
    const int scroll = argc > 3 ? std::atoi(argv[3]) : 0;

    // Tall viewport so nothing scrolls out of the dump. panel_viewport_h()
    // reads LINES when there is no tty, which is exactly this case.
#ifdef _WIN32
    _putenv_s("LINES", "120");
#else
    setenv("LINES", "120", 1);
#endif

    Model m = realistic_thread();
    m.ui.panel.descend(pn::Stats{});

    // Fold once so availability is known, then walk the visible tabs — the
    // same set the strip draws, so this tool cannot show a tab the panel
    // would not.
    auto* o = m.ui.panel.get<pn::Stats>();
    const auto& f = o->projection.refresh(m.d.current);

    for (auto t : stats::visible_tabs(f)) {
        if (!only.empty() && stats::tab_title(t) != only) continue;
        o->tab = t;
        o->scroll.y = scroll;
        std::printf("\n\x1b[1m── %s ──\x1b[0m  %s\n\n",
                    std::string{stats::tab_title(t)}.c_str(),
                    std::string{stats::tab_subtitle(t)}.c_str());
        dump(m, w, scroll);
    }
    std::printf("\n");
    return 0;
}
