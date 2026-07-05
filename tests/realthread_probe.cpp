// realthread_probe — load an actual on-disk thread JSON and time the
// resume hot path (rehydrate_frozen + first full render). The synthetic
// o1_probe / long_session_bench shapes don't reproduce the 30s open seen
// on real tool-heavy threads, so this probe runs the SAME code over real
// bytes to find where the time actually goes.
//
// Usage: ./realthread_probe /path/to/thread.json

#include <chrono>
#include <cstdio>

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/io/persistence.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using namespace std::chrono;
using agentty::Model;

static double ms(steady_clock::duration d) {
    return duration_cast<duration<double, std::milli>>(d).count();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s /path/to/thread.json\n", argv[0]);
        return 2;
    }
    auto load0 = steady_clock::now();
    auto loaded = agentty::persistence::load_thread_file(argv[1]);
    auto load1 = steady_clock::now();
    if (!loaded) {
        std::fprintf(stderr, "failed to load %s\n", argv[1]);
        return 1;
    }
    std::printf("load_thread_file        : %8.1f ms\n", ms(load1 - load0));

    Model m;
    m.d.current = std::move(*loaded);
    std::size_t tc = 0;
    for (auto& msg : m.d.current.messages) tc += msg.tool_calls.size();
    std::printf("thread: %zu messages, %zu tool_calls\n",
                m.d.current.messages.size(), tc);

    auto t0 = steady_clock::now();
    agentty::app::detail::rehydrate_frozen(m);
    auto t1 = steady_clock::now();
    std::printf("rehydrate_frozen        : %8.1f ms  (frozen=%zu rows=%zu)\n",
                ms(t1 - t0), m.ui.frozen.size(), m.ui.frozen.row_total());

    maya::StylePool pool;
    auto build = [&] {
        return maya::AppLayout{{
            .thread        = agentty::ui::thread_config(m),
            .changes_strip = agentty::ui::changes_strip_config(m),
            .composer      = agentty::ui::composer_config(m),
            .status_bar    = agentty::ui::status_bar_config(m),
            .overlay       = std::nullopt,
        }}.build();
    };
    auto build_and_render = [&](const char* tag, maya::Canvas& canvas) {
        auto a = steady_clock::now();
        auto root = build();
        canvas.clear();
        maya::render_tree(root, canvas, pool, maya::theme::dark,
                          /*auto_height=*/true);
        auto b = steady_clock::now();
        std::printf("%-24s: %8.1f ms\n", tag, ms(b - a));
    };
    // Canvas sized to the REAL inline canvas: frozen rows + a couple
    // screens of chrome — NOT 200000 rows (clear() of 40M cells dwarfs
    // the render and hides what we're measuring).
    const int canvas_h = static_cast<int>(m.ui.frozen.row_total()) + 200;
    maya::Canvas canvas(200, std::max(400, canvas_h), &pool);
    // Cold: nothing in the component cache yet.
    build_and_render("first render (cold)", canvas);
    // Warm: the cache captured the frozen Turn's cells on the cold
    // pass; the second render should blit them.
    build_and_render("second render (warm)", canvas);
    return 0;
}
