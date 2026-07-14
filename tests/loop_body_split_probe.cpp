// loop_body_split_probe — isolate WHERE the per-frame CPU actually goes in
// the REAL maya run<P> loop body during a long streaming turn.
//
// stream_cpu_probe proved rt.render() is flat & ~4% of a core. long_session_
// bench proved the render_key fold and view_build are sub-µs. Yet the user
// reports ~50% CPU on very long turns. This probe reproduces the EXACT work
// the run<P> frame gate does every RAF frame and times each phase separately:
//
//   1. AgenttyApp::visual_hash(model)   — the render gate
//   2. AgenttyApp::subscribe(model)     — get_sub() rebuild
//   3. AgenttyApp::view(model)          — Element tree build
//   4. rt.render(view_root)             — clear + render_tree + compose + wire
//
// so we can see which one (if any) scales with a growing live turn. Runs
// under a real PTY so rt.render is faithful.
//
// Run: ./build/loop_body_split_probe
//      PROBE_W=143 PROBE_H=75 PROBE_FRAMES=1200 ./build/loop_body_split_probe

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <string>
#include <vector>

#include <fcntl.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <unistd.h>

#include <maya/app/app.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/render/renderer.hpp>

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/view.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ToolUse;
using Clock = std::chrono::steady_clock;

static double ms(Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d)
        .count();
}
static void drain(int fd) {
    char buf[16384];
    while (::read(fd, buf, sizeof(buf)) > 0) {}
}
static std::size_t drain_count(int fd) {
    char buf[16384];
    std::size_t total = 0;
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) total += (std::size_t)n;
    return total;
}
static void tick(int t = 16) { maya::testing::advance_anim_clock_ms(t); }

static std::string para(int i) {
    return "\n\nParagraph " + std::to_string(i)
         + ": the refactor threads the new provider through the login flow, "
           "updating every caller and surfacing init errors as a Result so "
           "the CLI can report them instead of crashing at startup.";
}

struct Split { std::vector<double> hash, sub, view, render; };

static void report(const char* name, std::vector<double>& s, int frames) {
    std::sort(s.begin(), s.end());
    auto pct = [&](double p) {
        return s[std::min(s.size() - 1, (std::size_t)(p * (s.size() - 1)))];
    };
    double sum = 0; for (double v : s) sum += v;
    const std::size_t q = s.size() / 4;
    // NOTE: s is now sorted, so growth uses the ORIGINAL order — recomputed
    // by the caller. Here we only print aggregate stats.
    std::fprintf(stderr,
        "  %-10s mean=%.4f  p50=%.4f  p95=%.4f  p99=%.4f  max=%.4f ms\n",
        name, sum / s.size(), pct(0.50), pct(0.95), pct(0.99), s.back());
    (void)frames; (void)q;
}

static void growth(const char* name, const std::vector<double>& t) {
    const std::size_t q = t.size() / 4;
    if (!q) return;
    double first = 0, last = 0;
    for (std::size_t i = 0; i < q; ++i) first += t[i];
    for (std::size_t i = t.size() - q; i < t.size(); ++i) last += t[i];
    std::fprintf(stderr, "  %-10s first-q=%.4f  last-q=%.4f ms  (%.1fx)\n",
                 name, first / q, last / q, first > 0 ? last / first : 0.0);
}

int main() {
    setlocale(LC_ALL, "C.UTF-8");
    const int W = std::getenv("PROBE_W") ? std::atoi(std::getenv("PROBE_W")) : 120;
    const int H = std::getenv("PROBE_H") ? std::atoi(std::getenv("PROBE_H")) : 50;
    const int frames =
        std::getenv("PROBE_FRAMES") ? std::atoi(std::getenv("PROBE_FRAMES")) : 800;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed\n"); return 2;
    }
    struct winsize ws{};
    ws.ws_col = (unsigned short)W; ws.ws_row = (unsigned short)H;
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, STDIN_FILENO); dup2(slave, STDOUT_FILENO); close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    maya::RunConfig cfg; cfg.mode = maya::Mode::Inline;
    auto rt_r = maya::detail::Runtime::create(cfg);
    if (!rt_r) { std::fprintf(stderr, "Runtime::create failed\n"); return 2; }
    auto rt = std::move(*rt_r);

    Model m;
    m.d.current.id = agentty::ThreadId{"split-probe"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    auto do_render = [&] { (void)rt.render(agentty::ui::view(m)); drain(master); };
    do_render();

    // Deep backdrop.
    const int backdrop_turns = 8;
    for (int t = 0; t < backdrop_turns; ++t) {
        Message u; u.role = Role::User;
        u.text = "turn " + std::to_string(t) + ": explain and write the file";
        m.d.current.messages.push_back(std::move(u));
        Message a; a.role = Role::Assistant;
        std::string body; for (int p = 0; p < 6; ++p) body += para(p);
        a.text = std::move(body);
        ToolUse tc; tc.id = agentty::ToolCallId{"bd-" + std::to_string(t)};
        tc.name = agentty::ToolName{"write"};
        std::string content;
        for (int l = 0; l < 400; ++l)
            content += "    line " + std::to_string(l) + " of the file;\n";
        tc.args = nlohmann::json{{"file_path", "src/auth/login.cpp"},
                                 {"content", content}};
        tc.status = ToolUse::Done{Clock::now(), Clock::now(), "wrote 400 lines"};
        a.tool_calls.push_back(std::move(tc));
        m.d.current.messages.push_back(std::move(a));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner))
            rt.commit_inline_prefix(c->rows);
        do_render();
    }

    {
        Message u; u.role = Role::User;
        u.text = "now stream a very long answer explaining the whole design";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner))
            rt.commit_inline_prefix(c->rows);
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        do_render();
    }

    Message live; live.role = Role::Assistant;
    live.streaming_text = "Opening the explanation.";
    m.d.current.messages.push_back(std::move(live));

    std::fprintf(stderr,
        "backdrop ready: %zu msgs, frozen entries=%zu, frozen rows=%zu\n",
        m.d.current.messages.size(), m.ui.frozen.size(),
        (std::size_t)m.ui.frozen.row_total());

    Split sp;
    sp.hash.reserve(frames); sp.sub.reserve(frames);
    sp.view.reserve(frames); sp.render.reserve(frames);
    std::vector<double> emit_bytes; emit_bytes.reserve(frames);

    volatile std::uint64_t sink_hash = 0;

    for (int f = 0; f < frames; ++f) {
        m.d.current.messages.back().streaming_text += para(f);
        tick();

        // 1. visual_hash
        auto t0 = Clock::now();
        std::uint64_t h = agentty::app::AgenttyApp::visual_hash(m);
        auto t1 = Clock::now();
        sink_hash ^= h;

        // 2. subscribe
        auto sub = agentty::app::subscribe(m);
        auto t2 = Clock::now();
        (void)sub;

        // 3. view
        auto view_root = agentty::ui::view(m);
        auto t3 = Clock::now();

        // 4. render
        (void)rt.render(std::move(view_root));
        std::size_t bytes = drain_count(master);
        auto t4 = Clock::now();

        emit_bytes.push_back((double)bytes);

        sp.hash.push_back(ms(t1 - t0));
        sp.sub.push_back(ms(t2 - t1));
        sp.view.push_back(ms(t3 - t2));
        sp.render.push_back(ms(t4 - t3));
    }
    (void)sink_hash;

    // Keep unsorted copies for growth analysis.
    auto hash_t = sp.hash, sub_t = sp.sub, view_t = sp.view, render_t = sp.render;

    double tot = 0;
    for (int f = 0; f < frames; ++f)
        tot += hash_t[f] + sub_t[f] + view_t[f] + render_t[f];
    const double per_frame = tot / frames;

    std::fprintf(stderr, "\nper-phase per-frame cost over %d frames:\n", frames);
    report("visual_hash", sp.hash, frames);
    report("subscribe",   sp.sub, frames);
    report("view",        sp.view, frames);
    report("render",      sp.render, frames);
    std::fprintf(stderr, "  ----------\n");
    std::fprintf(stderr, "  TOTAL loop-body mean = %.4f ms/frame  "
                 "(=%.1f%% of one core at 60fps)\n",
                 per_frame, per_frame / 16.0 * 100.0);

    std::fprintf(stderr, "\ngrowth (first-quarter vs last-quarter):\n");
    growth("visual_hash", hash_t);
    growth("subscribe",   sub_t);
    growth("view",        view_t);
    growth("render",      render_t);
    growth("emit_bytes",  emit_bytes);

    {
        double bsum = 0; for (double b : emit_bytes) bsum += b;
        std::fprintf(stderr, "  emit_bytes mean = %.0f B/frame  "
                     "(=%.0f KB/s at 60fps)\n",
                     bsum / frames, bsum / frames * 60.0 / 1024.0);
    }

    close(master);
    return 0;
}
