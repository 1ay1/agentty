// thread_switch_prof_probe — where does a thread SWITCH actually spend
// its time, on the UI thread?
//
// The load already runs on a worker (cmd_factory::load_thread_async uses
// task_isolated), so the parse cost is NOT what the user waits on. What
// they wait on is the ThreadLoaded reducer plus the first render, both of
// which are on the UI thread and both of which run while the screen is
// frozen.
//
// This measures that path directly: move a loaded Thread into the model,
// clear the caches, rehydrate the frozen prefix, build the conversation
// element, and render it — the same sequence, in the same order, as the
// reducer. Reporting each stage separately is the point; a total tells
// you nothing about which part to fix.
//
//   ./build/agentty_standalone_tests thread_switch_prof_probe <thread> [width]
//
// Accepts .json or .jsonl so the same thread can be measured either side
// of the migration. No argument = no-op pass.

#include <agentty/domain/session.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>
#include <agentty/runtime/model.hpp>
#include <agentty/runtime/view/thread/conversation.hpp>
#include <agentty/runtime/app/update/internal.hpp>

#include <maya/render/renderer.hpp>
#include <maya/widget/conversation.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

// Median of a few runs — one sample of a millisecond-scale operation is
// mostly scheduler noise.
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("thread_switch_prof_probe: no thread given, nothing to do.\n"
                    "usage: %s <thread.json|thread.jsonl> [width]\n", argv[0]);
        return 0;
    }
    const fs::path p = argv[1];
    const int width = (argc > 2) ? std::atoi(argv[2]) : 120;

    // ── Off the UI thread: the load. Timed, but NOT part of the wait. ──
    auto t = clk::now();
    std::optional<Thread> loaded;
    if (p.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(p)) loaded = log->load_thread();
    } else {
        if (auto r = persistence::load_thread_file(p)) loaded = std::move(*r);
    }
    const double load_ms = ms_since(t);
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", p.string().c_str());
        return 1;
    }

    std::printf("%s\n  %zu messages, %.1f MB on disk\n",
                p.filename().string().c_str(), loaded->messages.size(),
                static_cast<double>(fs::file_size(p)) / 1e6);
    std::printf("\n  worker thread (user does NOT wait):\n");
    std::printf("    load + parse            %7.2f ms\n", load_ms);

    // ── On the UI thread: everything the reducer does. ────────────────
    std::vector<double> swap_v, rehyd_v, cfg_v, build_v, render_v;
    constexpr int kRuns = 5;
    for (int i = 0; i < kRuns; ++i) {
        Model m;
        Thread copy = *loaded;          // the reducer moves an owned Thread

        t = clk::now();
        m.d.current = std::move(copy);
        m.ui.view_cache.clear();
        swap_v.push_back(ms_since(t));

        t = clk::now();
        app::detail::rehydrate_frozen(m);
        rehyd_v.push_back(ms_since(t));

        t = clk::now();
        auto cfg = ui::conversation_config(m);
        cfg_v.push_back(ms_since(t));

        t = clk::now();
        auto el = maya::Conversation{std::move(cfg)}.build();
        build_v.push_back(ms_since(t));

        t = clk::now();
        const std::string out = maya::render_to_string(el, width);
        render_v.push_back(ms_since(t));
        if (i == 0)
            std::printf("\n  UI thread (this IS the wait), width %d:\n", width);
    }

    const double swap   = median(swap_v);
    const double rehyd  = median(rehyd_v);
    const double cfg    = median(cfg_v);
    const double build  = median(build_v);
    const double render = median(render_v);

    std::printf("    model swap + cache drop %7.2f ms\n", swap);
    std::printf("    rehydrate_frozen        %7.2f ms\n", rehyd);
    std::printf("    conversation_config     %7.2f ms\n", cfg);
    std::printf("    element build           %7.2f ms\n", build);
    std::printf("    render                  %7.2f ms\n", render);
    std::printf("    %-23s %7.2f ms  <- perceived switch cost\n",
                "TOTAL on UI thread", swap + rehyd + cfg + build + render);

    // The number that decides whether windowed reads are worth it: how
    // much of the UI-thread cost scales with THREAD length rather than
    // SCREEN size. rehydrate_frozen is already bounded; the model swap is
    // a move plus a cache clear, both O(1)-ish; the rest is per-visible-row.
    // The number that decides whether windowed reads are worth it: how
    // much of the UI-thread cost scales with THREAD length rather than
    // SCREEN size.
    {
        Model m;
        m.d.current = *loaded;
        app::detail::rehydrate_frozen(m);
        std::printf("\n  frozen: %zu entries, %zu rows, through %zu of %zu "
                    "messages\n", m.ui.frozen.size(), m.ui.frozen.row_total(),
                    m.ui.frozen_through, loaded->messages.size());
    }
    return 0;
}
