// Theme-browser INPUT LAG probe.
//
// theme_switch_leak_probe proved there is no leak: cost per switch is flat
// forever. But flat-and-expensive still feels laggy if keys arrive faster
// than a switch costs — holding ↓ on key-repeat delivers ~30 keys/sec, and
// every one of them is dispatched and re-rendered.
//
// This measures the thing the user actually experiences: hold the key for N
// seconds, then ask how far BEHIND the UI is when you let go. Latency, not
// throughput — the number that decides whether the browser feels instant or
// mushy.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/view/view.hpp"

#include "agentty/runtime/app/update/internal.hpp"   // rehydrate_frozen
#include "agentty/io/persistence.hpp"

#include <maya/core/render_context.hpp>
#include <maya/app/inline.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace agentty;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    // Typical X11/wayland key-repeat: ~25-30 chars/sec after the initial delay.
    const int    keys_per_sec = argc > 1 ? std::atoi(argv[1]) : 30;
    const double hold_secs    = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int    msgs         = argc > 3 ? std::atoi(argv[3]) : 100;

    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-lag"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    Model m;
    // A REAL thread when one is named (argv[4] or AGENTTY_THREAD), else a
    // synthetic one. The difference matters more than it looks: frozen state
    // is established by rehydrate_frozen, and a transcript that is mostly
    // FROZEN renders from prebuilt Elements while the live tail does not.
    // Measuring with everything live would blame the renderer for work no
    // real session performs.
    const char* thread_path = argc > 4 ? argv[4] : std::getenv("AGENTTY_THREAD");
    if (thread_path && *thread_path) {
        auto t = persistence::load_thread_file(thread_path);
        if (t) {
            m.d.current = std::move(*t);
            std::printf("loaded %s: %zu messages\n",
                        thread_path, m.d.current.messages.size());
        } else {
            std::printf("could not load %s (err %d) - falling back\n",
                        thread_path, (int)t.error().kind);
        }
    }
    if (m.d.current.messages.empty()) {
        for (int i = 0; i < msgs; ++i) {
            Message msg;
            msg.role = (i % 2) ? Role::Assistant : Role::User;
            msg.text = "## Heading " + std::to_string(i) +
                "\n\nSome prose with `code` and *emphasis*, long enough to wrap "
                "across a couple of lines in a normal terminal width.\n\n"
                "```cpp\nint f" + std::to_string(i) + "() { return 42; }\n```\n";
            m.d.current.messages.push_back(std::move(msg));
        }
    }
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;

    // A REAL session has most of its transcript frozen: m.ui.frozen holds
    // already-built Elements, and only the last couple of screens are live
    // tail rebuilt each frame. Leaving frozen_through at 0 makes the WHOLE
    // transcript live, which measures a state no user is ever in — and
    // exaggerates the cost linearly in conversation length.
    //
    // rehydrate_frozen() is what a theme change calls, and it re-seals from
    // scratch under a row budget, so let it establish the realistic split
    // rather than hand-seeding an index it would immediately overwrite.
    // rehydrate_frozen() is what a theme change calls, and it re-seals from
    // scratch under a row budget — so call it directly rather than hoping a
    // headless render establishes the split (it does not; freezing is driven
    // by the runtime's commit path, which no probe reaches).
    app::detail::rehydrate_frozen(m);
    std::printf("frozen: through=%zu of %zu msgs, %zu prebuilt rows\n",
                m.ui.frozen_through, m.d.current.messages.size(),
                m.ui.frozen.size());

    // One ↓ the way the event loop does it: dispatch, then paint.
    //
    // Split on purpose. maya's Program loop already coalesces RENDERS —
    // dispatch only sets needs_render, and one frame is painted per batch —
    // but it calls drain_pending() per EVENT, so the reducer runs once per
    // key. If the reduce half dominates, coalescing renders buys nothing and
    // the fix has to be in the reducer path.
    double last_reduce_ms = 0, last_render_ms = 0;
    const auto one_key = [&] {
        auto r0 = clk::now();
        m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
        last_reduce_ms = std::chrono::duration<double, std::milli>(clk::now() - r0).count();
        auto p0 = clk::now();
        maya::RenderContext ctx{100, 40, maya::render_generation(), true};
        maya::RenderContextGuard g(ctx);
        auto el = ui::view(m);
        (void)maya::render_to_string(el, 100);
        last_render_ms = std::chrono::duration<double, std::milli>(clk::now() - p0).count();
    };

    // Warm the caches so we measure steady state, not first-paint.
    for (int i = 0; i < 5; ++i) one_key();

    auto t0 = clk::now();
    one_key();
    const double per_key_ms =
        std::chrono::duration<double, std::milli>(clk::now() - t0).count();

    const int    total_keys   = static_cast<int>(keys_per_sec * hold_secs);
    const double key_period_ms = 1000.0 / keys_per_sec;
    const double work_ms       = per_key_ms * total_keys;
    const double wall_ms       = hold_secs * 1000.0;

    std::printf("transcript=%d msgs   key-repeat=%d/s   hold=%.1fs "
                "(%d keys)\n\n", msgs, keys_per_sec, hold_secs, total_keys);
    std::printf("  cost of ONE \u2193 (reduce+render) : %.2f ms"
                "   (reduce %.2f + render %.2f)\n",
                per_key_ms, last_reduce_ms, last_render_ms);
    // What coalescing RENDERS alone would leave: the reducer still runs per
    // key, so this is the floor any render-only fix can reach.
    std::printf("  if renders coalesced          : %.2f ms/key "
                "(reduce only)\n", last_reduce_ms);
    std::printf("  budget per key at %2d/s        : %.2f ms\n",
                keys_per_sec, key_period_ms);

    if (per_key_ms <= key_period_ms) {
        std::printf("\n  ✓ keeps up (%.0f%% of budget) — browser feels instant\n",
                    100.0 * per_key_ms / key_period_ms);
        return 0;
    }

    // Every key costs more than its slot, so the backlog grows for as long as
    // the key is held. THIS is what "laggy after a while" means: not a leak,
    // a queue.
    const double overrun_ms = work_ms - wall_ms;
    std::printf("\n  ✗ CANNOT keep up: %.1fx over budget\n",
                per_key_ms / key_period_ms);
    std::printf("    work queued in %.1fs hold  : %.0f ms\n", hold_secs, work_ms);
    std::printf("    wall time available        : %.0f ms\n", wall_ms);
    std::printf("    BACKLOG when you let go    : %.0f ms  <- felt as lag\n",
                overrun_ms);
    std::printf("    themes still to scroll past: %.0f\n",
                overrun_ms / per_key_ms);
    return 0;
}
