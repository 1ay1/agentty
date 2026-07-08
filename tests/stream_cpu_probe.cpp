// stream_cpu_probe — measure the TRUE end-to-end per-frame cost of a live
// streaming turn through the REAL maya Runtime::render (inline compose path)
// under a PTY, with the REAL agentty view + freeze/trim pipeline.
//
// This is the missing measurement long_session_bench couldn't make: it
// times rt->render() (clear + render_tree + compose row-diff + wire emit),
// NOT just render_tree into a bare canvas. It reproduces the user report
// "30% CPU on long turns" by building a deep frozen backdrop, then streaming
// a long assistant turn frame-by-frame at the production 60 fps reveal
// cadence, printing per-frame render() ms as the reply grows.
//
// Run: ./build/stream_cpu_probe            (default 80x30, 400 stream frames)
//      PROBE_H=50 PROBE_W=120 ./build/stream_cpu_probe
//      PROBE_FRAMES=800 ./build/stream_cpu_probe

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <maya/app/app.hpp>
#include <maya/core/anim_clock.hpp>
#include <maya/render/renderer.hpp>

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

static void tick(int t = 16) { maya::testing::advance_anim_clock_ms(t); }

static agentty::ToolUse settled_edit(int idx, int n_hunks, int lines) {
    agentty::ToolUse t;
    t.id   = agentty::ToolCallId{"edit_" + std::to_string(idx)};
    t.name = agentty::ToolName{"edit"};
    nlohmann::json edits = nlohmann::json::array();
    std::string diff = "```diff\n";
    for (int h = 0; h < n_hunks; ++h) {
        std::string ot, nt;
        for (int l = 0; l < lines; ++l) {
            ot += "    const auto old_" + std::to_string(l) + " = compute(l);\n";
            nt += "    const auto new_" + std::to_string(l) + " = compute(l)+1;\n";
        }
        edits.push_back({{"old_text", ot}, {"new_text", nt}});
        diff += "@@ hunk " + std::to_string(h) + " @@\n";
        for (int l = 0; l < lines; ++l) {
            diff += "-    const auto old_" + std::to_string(l) + " = compute(l);\n";
            diff += "+    const auto new_" + std::to_string(l) + " = compute(l)+1;\n";
        }
    }
    diff += "```";
    t.args = nlohmann::json{{"file_path", "src/module_" + std::to_string(idx) + ".cpp"},
                           {"edits", edits}};
    auto now = Clock::now();
    t.status = agentty::ToolUse::Done{now - std::chrono::milliseconds{10}, now, diff};
    return t;
}

// A stable prose paragraph so growth is realistic (wraps a few rows).
static std::string para(int i) {
    return "\n\nParagraph " + std::to_string(i)
         + ": the refactor threads the new provider through the login flow, "
           "updating every caller and surfacing init errors as a Result so "
           "the CLI can report them instead of crashing at startup.";
}

int main() {
    setlocale(LC_ALL, "C.UTF-8");
    const int W = std::getenv("PROBE_W") ? std::atoi(std::getenv("PROBE_W")) : 80;
    const int H = std::getenv("PROBE_H") ? std::atoi(std::getenv("PROBE_H")) : 30;
    const int frames =
        std::getenv("PROBE_FRAMES") ? std::atoi(std::getenv("PROBE_FRAMES")) : 400;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed\n");
        return 2;
    }
    struct winsize ws{};
    ws.ws_col = (unsigned short)W;
    ws.ws_row = (unsigned short)H;
    ioctl(slave, TIOCSWINSZ, &ws);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    close(slave);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    maya::RunConfig cfg;
    cfg.mode = maya::Mode::Inline;
    auto rt_r = maya::detail::Runtime::create(cfg);
    if (!rt_r) {
        std::fprintf(stderr, "Runtime::create failed\n");
        return 2;
    }
    auto rt = std::move(*rt_r);

    Model m;
    m.d.current.id = agentty::ThreadId{"cpu-probe"};
    m.d.available_models.push_back({});
    m.d.available_models.back().id = agentty::ModelId{"claude-opus-4-1"};
    agentty::app::detail::clear_frozen(m);

    auto render = [&] {
        auto t0 = Clock::now();
        (void)rt.render(agentty::ui::view(m));
        auto t1 = Clock::now();
        drain(master);
        return ms(t1 - t0);
    };

    // Welcome frame.
    render();

    // ── Build a DEEP frozen backdrop: several settled prose+write turns so
    //    the frozen prefix is tall (thousands of rows), matching a long
    //    session. Each turn is frozen so it blits.
    const int backdrop_turns = 8;
    for (int t = 0; t < backdrop_turns; ++t) {
        Message u;
        u.role = Role::User;
        u.text = "turn " + std::to_string(t) + ": explain and write the file";
        m.d.current.messages.push_back(std::move(u));
        Message a;
        a.role = Role::Assistant;
        std::string body;
        for (int p = 0; p < 6; ++p) body += para(p);
        a.text = std::move(body);
        // A settled write tool with a chunky body.
        ToolUse tc;
        tc.id = agentty::ToolCallId{"bd-" + std::to_string(t)};
        tc.name = agentty::ToolName{"write"};
        std::string content;
        for (int l = 0; l < 400; ++l)
            content += "    line " + std::to_string(l) + " of the file;\n";
        tc.args = nlohmann::json{{"file_path", "src/auth/login.cpp"},
                                 {"content", content}};
        tc.status = ToolUse::Done{Clock::now(), Clock::now(),
                                  "wrote 400 lines"};
        a.tool_calls.push_back(std::move(tc));
        m.d.current.messages.push_back(std::move(a));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner))
            rt.commit_inline_prefix(c->rows);
        render();
    }

    std::fprintf(stderr,
        "backdrop ready: %zu msgs, frozen entries=%zu, frozen rows=%zu, "
        "maya content rows=%d\n",
        m.d.current.messages.size(), m.ui.frozen.size(),
        (std::size_t)m.ui.frozen.row_total(), rt.inline_content_rows());

    // ── Now the LIVE streaming turn: user submit, then grow the assistant
    //    body one paragraph per rendered frame at the 60 fps reveal cadence.
    {
        Message u;
        u.role = Role::User;
        u.text = "now stream a very long answer explaining the whole design";
        m.d.current.messages.push_back(std::move(u));
        agentty::app::detail::freeze_through(m, m.d.current.messages.size());
        auto trim = agentty::app::detail::trim_frozen_if_oversized(m);
        using Cmd = maya::Cmd<agentty::Msg>;
        if (const auto* c = std::get_if<Cmd::CommitScrollback>(&trim.inner))
            rt.commit_inline_prefix(c->rows);
        m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
        render();
    }

    // PROBE_EDITS>0: the live turn accumulates N settled edit tools with a
    // running bash tool at the tail (panel NOT cached; per-event edit bodies
    // SHOULD cache-blit). Drives the REAL grow-and-retry Runtime so we see
    // whether tall edit bodies overflow the canvas and miss the cache.
    const int n_edits =
        std::getenv("PROBE_EDITS") ? std::atoi(std::getenv("PROBE_EDITS")) : 0;
    const int edit_lines =
        std::getenv("PROBE_EDIT_LINES") ? std::atoi(std::getenv("PROBE_EDIT_LINES"))
                                        : 120;

    Message live;
    live.role = Role::Assistant;
    if (n_edits > 0) {
        live.text = "Applying the refactor across the modules.";
        agentty::ToolUse bash;
        bash.id   = agentty::ToolCallId{"bash_live"};
        bash.name = agentty::ToolName{"bash"};
        bash.args = nlohmann::json{{"command", "cmake --build build -j10"}};
        bash.status = agentty::ToolUse::Running{Clock::now()};
        live.tool_calls.push_back(std::move(bash));
    } else {
        live.streaming_text = "Opening the explanation.";
    }
    m.d.current.messages.push_back(std::move(live));

    std::vector<double> samp;
    std::vector<double> timeline;
    samp.reserve(frames);
    timeline.reserve(frames);
    // Track ComponentElement render() cache-MISSES per streaming frame — the
    // hard proof that the committed prefix stays cached (a hit never bumps
    // this). Steady frames should miss only the handful of live-tail /
    // newest-block components, NOT the O(N) committed prefix.
    std::uint64_t miss_prev = maya::render_detail::component_render_calls();
    std::uint64_t miss_worst = 0;
    std::uint64_t miss_worst_steady = 0;
    std::uint64_t miss_sum = 0;
    // Buckets: report cost as the reply grows so we can see any scaling.
    for (int f = 0; f < frames; ++f) {
        if (n_edits > 0) {
            // Grow the edit turn: insert a fresh settled edit before the
            // running bash tool, up to n_edits, then just re-render (the
            // running spinner keeps the panel live/uncached every frame).
            auto& tcs = m.d.current.messages.back().tool_calls;
            if ((int)tcs.size() - 1 < n_edits) {
                tcs.insert(tcs.end() - 1,
                           settled_edit((int)tcs.size() - 1, 2, edit_lines));
            }
        } else {
            m.d.current.messages.back().streaming_text += para(f);
        }
        tick();
        double r = render();
        samp.push_back(r);
        timeline.push_back(r);
        const std::uint64_t now = maya::render_detail::component_render_calls();
        const std::uint64_t d = now - miss_prev;
        miss_prev = now;
        miss_sum += d;
        if (d > miss_worst) miss_worst = d;
        // The first 1-2 frames legitimately miss the whole pre-built
        // backdrop + committed prefix for the FIRST time (one-time cache
        // warm at turn start; whether it lands on frame 0 or 1 depends on
        // when the streaming content first appears vs the backdrop paint).
        // Every frame from 2 on must miss only a small constant (live tail
        // + reveal decoration), never the O(N) committed prefix — that is
        // the steady-state O(1)-per-frame guarantee.
        if (f >= 2 && d > miss_worst_steady) miss_worst_steady = d;
    }

    std::sort(samp.begin(), samp.end());
    auto pct = [&](double p) {
        return samp[std::min(samp.size() - 1,
                             (std::size_t)(p * (samp.size() - 1)))];
    };
    double sum = 0;
    for (double v : samp) sum += v;

    std::fprintf(stderr,
        "\nstreaming render() over %d frames (final live rows=%d):\n"
        "  mean=%.3f  p50=%.3f  p95=%.3f  p99=%.3f  max=%.3f ms/frame\n",
        frames, rt.inline_content_rows(),
        sum / samp.size(), pct(0.50), pct(0.95), pct(0.99), samp.back());

    // Effective CPU at the 60 fps reveal cadence (16 ms budget).
    std::fprintf(stderr,
        "  at 60fps: mean render = %.1f%% of one core (%.2f ms/16 ms budget)\n",
        (sum / samp.size()) / 16.0 * 100.0, sum / samp.size());

    // Component render() cache-miss rate — the O(1)-prefix proof. If the
    // committed prefix stayed cached, per-frame misses are a small constant
    // (live tail + newest blocks) independent of transcript length. A worst
    // frame in the hundreds would mean the prefix re-rendered.
    std::fprintf(stderr,
        "  component render() MISSES: mean=%.1f/frame  worst=%llu/frame  "
        "total=%llu over %d frames\n"
        "  POST-WARM (frame>=2) worst misses = %llu/frame  "
        "(committed prefix stays cached \xe2\x86\x92 O(1)/frame)\n",
        double(miss_sum) / double(frames),
        (unsigned long long)miss_worst, (unsigned long long)miss_sum, frames,
        (unsigned long long)miss_worst_steady);

    // First-quarter vs last-quarter mean, to expose growth with length.
    const std::size_t q = timeline.size() / 4;
    if (q > 0) {
        double first = 0, last = 0;
        for (std::size_t i = 0; i < q; ++i) first += timeline[i];
        for (std::size_t i = timeline.size() - q; i < timeline.size(); ++i)
            last += timeline[i];
        std::fprintf(stderr,
            "  growth: first-quarter mean=%.3f  last-quarter mean=%.3f ms "
            "(%.1fx)\n",
            first / q, last / q, (first > 0 ? (last / first) : 0.0));
    }
    close(master);
    return 0;
}
