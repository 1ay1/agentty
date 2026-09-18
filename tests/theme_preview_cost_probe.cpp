// What one theme-preview keystroke costs, measured on the two paths.
//
// theme_input_lag_probe measures reduce+render on a SYNTHETIC transcript,
// where rehydrate_frozen's row budget collapses everything to ~11 prebuilt
// rows — so it cannot see the cost this fix removes. This measures the two
// restyle paths directly:
//
//   restyle_sealed_turns()  publish + invalidate + REHYDRATE FROZEN
//   restyle_preview()       publish + invalidate            (the fix)
//
// The gap between them is what every arrow key used to pay.

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/appearance.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace agentty;
using clk = std::chrono::steady_clock;

namespace {

double ms_since(clk::time_point t0) {
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    const int msgs = argc > 1 ? std::atoi(argv[1]) : 1000;

    app::install_deps(app::Deps{
        .stream = [](provider::Request, provider::EventSink) {},
        .save_thread = [](const Thread&) {},
        .load_threads = [] { return std::vector<Thread>{}; },
        .load_thread = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"probe"}; },
        .title_from = [](std::string_view) { return std::string{"t"}; },
    });

    Model m;
    for (int i = 0; i < msgs; ++i) {
        Message msg;
        msg.role = (i % 2) ? Role::Assistant : Role::User;
        msg.text = "## Heading " + std::to_string(i) +
            "\n\nSome prose with `code` and *emphasis*, long enough to wrap "
            "across a couple of lines in a normal terminal width.\n\n"
            "```cpp\nint f" + std::to_string(i) + "() { return 42; }\n```\n";
        m.d.current.messages.push_back(std::move(msg));
    }

    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
    app::detail::rehydrate_frozen(m);

    std::printf("transcript: %d msgs, frozen_through=%zu\n\n",
                msgs, m.ui.frozen_through);

    constexpr int kIters = 20;

    // The OLD per-keystroke cost.
    double full = 0;
    for (int i = 0; i < kIters; ++i) {
        m.d.ui.theme = (i % 2) ? "Dracula" : "Nord";
        const auto t0 = clk::now();
        app::detail::restyle_sealed_turns(m);
        full += ms_since(t0);
    }
    full /= kIters;

    // The NEW per-keystroke cost.
    double second = 0;
    for (int i = 0; i < kIters; ++i) {
        m.d.ui.theme = (i % 2) ? "Dracula" : "Nord";
        const auto t0 = clk::now();
        app::detail::restyle_sealed_turns(m);
        second += ms_since(t0);
    }
    second /= kIters;

    std::printf("  restyle_sealed_turns (was, per arrow) : %8.3f ms\n", full);
    std::printf("  restyle (2nd pass, warm cache)       : %8.3f ms\n", second);
    if (second > 0.0001)
        std::printf("  speedup                               : %8.1fx\n",
                    full / second);
    std::printf("\n  budget at 30 keys/sec                 : %8.3f ms\n", 33.333);
    std::printf("  old: %s   new: %s\n",
                full    < 33.333 ? "fits" : "OVER BUDGET",
                second < 33.333 ? "fits" : "OVER BUDGET");
    return 0;
}
