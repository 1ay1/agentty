// resize_prof_probe — what does a terminal RESIZE actually cost?
//
// A width change is the expensive kind: maya invalidates the previous
// cell grid (every row's wrap points move) and HardResets, so the next
// frame repaints the whole frozen canvas — up to frozen_row_budget()
// rows, which is 3 viewports.
//
// A thread switch renders ~2 screens ONCE. A resize repaints ~3
// viewports, and a drag emits a resize per column. This measures the
// per-resize cost at realistic terminal sizes, split so the answer says
// WHICH part to fix rather than just "it's slow".
//
//   ./build/agentty_standalone_tests resize_prof_probe <thread> [rows]
//
// Accepts .json or .jsonl. No argument = no-op pass.

#include <agentty/domain/session.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>
#include <agentty/runtime/model.hpp>
#include <agentty/runtime/view/thread/conversation.hpp>
#include <agentty/runtime/app/update/internal.hpp>

#include <maya/render/renderer.hpp>
#include <maya/widget/conversation.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("resize_prof_probe: no thread given, nothing to do.\n"
                    "usage: %s <thread.json|thread.jsonl> [rows]\n", argv[0]);
        return 0;
    }
    const fs::path p = argv[1];
    const int rows = (argc > 2) ? std::atoi(argv[2]) : 60;

    std::optional<Thread> loaded;
    if (p.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(p)) loaded = log->load_thread();
    } else {
        if (auto r = persistence::load_thread_file(p)) loaded = std::move(*r);
    }
    if (!loaded) {
        std::fprintf(stderr, "load failed: %s\n", p.string().c_str());
        return 1;
    }

    Model m;
    m.d.current = std::move(*loaded);
    app::detail::rehydrate_frozen(m);

    std::printf("%s: %zu messages\n", p.filename().string().c_str(),
                m.d.current.messages.size());
    std::printf("  frozen canvas: %zu entries, %zu rows (terminal %d rows)\n",
                m.ui.frozen.size(), m.ui.frozen.row_total(), rows);

    // A resize is a WIDTH change: every cached Element must re-layout at
    // the new width, and maya's component cache keys on width, so nothing
    // is reused. Alternating widths models a drag, where no width repeats.
    std::printf("\n  per-frame cost at a NEW width (what a resize pays):\n");
    for (int w : {120, 100, 80, 160}) {
        std::vector<double> build_v, render_v;
        for (int i = 0; i < 5; ++i) {
            // A fresh width each iteration so no cache can hit — the
            // pessimistic case a drag actually produces.
            const int width = w + i;
            auto t = clk::now();
            auto cfg = ui::conversation_config(m);
            auto el  = maya::Conversation{std::move(cfg)}.build();
            build_v.push_back(ms_since(t));

            t = clk::now();
            const std::string out = maya::render_to_string(el, width);
            render_v.push_back(ms_since(t));
        }
        std::printf("    width %3d: build %6.2f ms  render %6.2f ms  "
                    "total %6.2f ms\n",
                    w, median(build_v), median(render_v),
                    median(build_v) + median(render_v));
    }

    // The SAME width twice: this is what the component cache is for, and
    // the gap between the two numbers is what a resize throws away.
    std::printf("\n  same width twice (cache warm) \u2014 the steady state:\n");
    {
        std::vector<double> warm;
        auto cfg0 = ui::conversation_config(m);
        auto el0  = maya::Conversation{std::move(cfg0)}.build();
        (void)maya::render_to_string(el0, 120);
        for (int i = 0; i < 5; ++i) {
            auto t = clk::now();
            auto cfg = ui::conversation_config(m);
            auto el  = maya::Conversation{std::move(cfg)}.build();
            const std::string out = maya::render_to_string(el, 120);
            warm.push_back(ms_since(t));
        }
        std::printf("    width 120: %6.2f ms\n", median(warm));
    }

    return 0;
}
