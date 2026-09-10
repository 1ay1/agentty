// stats_refresh_bench — how much does the per-frame projection copy cost?
//
// Projection::refresh() rebuilds `view_` from `sealed_` on EVERY call, and
// the stats panel calls it twice per frame (once in the view, once in the
// StatsTab reducer). Facts owns ~6 double series capped at 256 entries plus
// several string-keyed tallies, so that assignment is a deep copy, not a
// memcpy.
//
// This is not a test of behaviour -- it prints numbers for a human. Kept as
// a NO_TEST probe so it builds with the suite and cannot bit-rot silently.

#include "agentty/domain/stats/facts.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/panels.hpp"

#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include <chrono>
#include <cstdio>
#include <string>

using namespace agentty;

namespace {

Message assistant_turn(int i) {
    Message m;
    m.role         = Role::Assistant;
    m.id           = MessageId{"m" + std::to_string(i)};
    m.served_model = ModelId{"claude-sonnet-4-5"};
    m.text         = "some assistant prose that is long enough to be realistic";
    Message::Telemetry t;
    t.input_tokens   = 1200;
    t.output_tokens  = 400;
    t.cache_read     = 15000;
    t.cache_creation = 200;
    t.ttft_ms        = 100;
    t.stream_ms      = 900;
    m.telemetry = t;
    return m;
}

}  // namespace

int main() {
    using namespace std::chrono;

    // A long session: the case where the copy is biggest, and exactly the
    // case a user is in when they open the panel to see what happened.
    Thread t;
    t.id = ThreadId{"bench"};
    for (int i = 0; i < 400; ++i) t.messages.push_back(assistant_turn(i));

    stats::Projection p;
    p.refresh(t);                       // warm: seal everything once

    constexpr int kIters = 2000;
    auto t0 = steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        const auto& f = p.refresh(t);
        asm volatile("" :: "r"(&f) : "memory");
    }
    auto t1 = steady_clock::now();

    const double us =
        duration_cast<nanoseconds>(t1 - t0).count() / 1000.0 / kIters;
    std::printf("settled thread, %zu messages\n", t.messages.size());
    std::printf("  refresh()      : %.1f us\n", us);
    std::printf("  x2 per frame   : %.1f us\n", us * 2);
    std::printf("  at 30fps       : %.1f%% of a 33ms frame\n",
                us * 2 / 33000.0 * 100.0);

    // The projection is only one part of a frame. What the user actually
    // waits for is the whole panel: build the element tree, lay it out,
    // paint it. Measure THAT, or the number above is a true fact about an
    // irrelevant thing.
    Model m;
    m.d.current = t;
    auto [opened, _] = app::update(std::move(m), Msg{OpenStats{}});
    m = std::move(opened);

    constexpr int kFrames = 300;
    maya::StylePool pool;
    maya::Canvas canvas(120, 60, &pool);

    auto t2 = steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        auto el = ui::stats_panel(m);
        maya::render_tree(el, canvas, pool, maya::theme::dark,
                          /*auto_height=*/true);
    }
    auto t3 = steady_clock::now();

    const double frame_us =
        duration_cast<nanoseconds>(t3 - t2).count() / 1000.0 / kFrames;
    std::printf("\nfull panel build + render at 120x60\n");
    std::printf("  per frame      : %.0f us\n", frame_us);
    std::printf("  budget at 60fps: 16600 us  (%.1f%% used)\n",
                frame_us / 16600.0 * 100.0);
    std::printf("  keypress->paint: %.0f us of input latency\n", frame_us);
    return 0;
}
