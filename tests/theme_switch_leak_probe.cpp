// Theme-switch degradation probe. Drives the REAL appearance reducer the way
// the browser does (↑/↓ through schemes) over a realistic transcript, and
// reports per-switch time plus process RSS every N switches.
//
// "Gets laggy over time" is the signature of unbounded growth, not slow code:
// if switch #500 costs what switch #1 did, there is no leak and the cost is
// simply per-switch work. If it climbs, something is accumulating.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/domain/ui_prefs.hpp"

#include <maya/style/schemes.hpp>
#include <maya/core/render_context.hpp>
#include <maya/app/inline.hpp>

#include "agentty/runtime/view/view.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;

static long rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    std::fclose(f);
    return kb;
}

int main(int argc, char** argv) {
    const int switches = argc > 1 ? std::atoi(argv[1]) : 600;
    const int msgs     = argc > 2 ? std::atoi(argv[2]) : 60;

    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-theme"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    // A transcript with enough prose that re-colouring is real work.
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

    // Every scheme name, so we cycle like the browser does.
    std::vector<std::string> names;
    for (const auto& s : maya::theme::schemes) names.emplace_back(s.name);
    std::printf("themes=%zu  transcript=%d msgs  switches=%d  frozen_through=%zu\n\n",
                names.size(), msgs, switches, m.ui.frozen_through);

    // Open the pane + browser, exactly as the user does, so the probe walks
    // the SAME reducer path as ↑/↓ in the theme list rather than a shortcut
    // that might skip the expensive part.
    //
    // frozen_through matters: a theme change re-seals the frozen scrollback
    // (rehydrate_frozen), and THAT is the expensive half. A probe that left
    // it at 0 would exercise only the cheap tail and report "no problem" on
    // a session that has one. Seed it the way a real conversation does —
    // most of the transcript sealed, a live tail still building.
    m.ui.frozen_through = m.d.current.messages.size() > 4
                        ? m.d.current.messages.size() - 4 : 0;

    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;

    const long rss0 = rss_kb();
    std::printf("%-8s %-12s %-12s %-12s %-10s\n",
                "switch", "us/switch", "us/render", "RSS kB", "\u0394RSS kB");

    using clk = std::chrono::steady_clock;
    const int bucket = switches / 12 > 0 ? switches / 12 : 1;
    double bucket_us = 0, bucket_render_us = 0;
    std::printf("(us/switch = reducer only; us/render = view()+render, what the "
                "user actually waits on)\n");
    for (int i = 0; i < switches; ++i) {
        auto t0 = clk::now();
        // One step down the browser list = one live preview = one re-seal.
        m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
        bucket_us += std::chrono::duration<double, std::micro>(clk::now() - t0).count();

        // …and then the frame. The reducer only marks what is stale; the
        // COST of a theme change is rebuilding the Elements it dropped, and
        // that happens here. A probe that stopped at the reducer would call
        // a re-parse of the whole transcript "free".
        auto t1 = clk::now();
        {
            maya::RenderContext ctx{100, 40, maya::render_generation(), true};
            maya::RenderContextGuard g(ctx);
            auto el = ui::view(m);
            (void)maya::render_to_string(el, 100);
        }
        bucket_render_us += std::chrono::duration<double, std::micro>(clk::now() - t1).count();

        if ((i + 1) % bucket == 0) {
            const long r = rss_kb();
            std::printf("%-8d %-12.0f %-12.0f %-12ld %+-10ld\n",
                        i + 1, bucket_us / bucket, bucket_render_us / bucket,
                        r, r - rss0);
            bucket_us = 0;
            bucket_render_us = 0;
        }
    }
    return 0;
}
