// What does one theme change actually COST the terminal?
//
// A cursor move repaints two rows. A theme change repaints every cell on
// screen — every colour changes, so maya's cell diff finds no unchanged
// spans and emits the whole viewport. Over a slow link, or a terminal that
// is slow to parse SGR, that is the difference between "instant" and
// "mushy" even when the CPU cost is a few milliseconds.
//
// Prints bytes written per keypress for a theme move vs a plain cursor
// move, so the two are directly comparable.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/view/view.hpp"

#include <maya/core/render_context.hpp>
#include <maya/print.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;
using clk = std::chrono::steady_clock;

namespace {

Model seeded(int msgs) {
    Model m;
    for (int i = 0; i < msgs; ++i) {
        Message msg;
        msg.role = (i % 2) ? Role::Assistant : Role::User;
        msg.text = "## Heading " + std::to_string(i) +
            "\n\nProse with `code` and *emphasis*, long enough to wrap across "
            "a couple of lines in a normal terminal.\n\n```cpp\nint f" +
            std::to_string(i) + "() { return 42; }\n```\n";
        m.d.current.messages.push_back(std::move(msg));
    }
    app::detail::rehydrate_frozen(m);
    return m;
}

// Render and return the emitted string — the bytes a terminal must parse.
std::string frame(const Model& m, int w = 100, int h = 40) {
    maya::RenderContext ctx{w, h, maya::render_generation(), true};
    maya::RenderContextGuard g(ctx);
    auto el = ui::view(m);
    return maya::render_to_string(el, w);
}

// How many bytes CHANGED between two frames — what a diffing writer sends.
std::size_t changed_bytes(const std::string& a, const std::string& b) {
    std::size_t p = 0;
    const std::size_t lim = std::min(a.size(), b.size());
    while (p < lim && a[p] == b[p]) ++p;
    std::size_t q = 0;
    while (q < lim - p && a[a.size()-1-q] == b[b.size()-1-q]) ++q;
    return b.size() - p - q;
}

}  // namespace

int main(int argc, char** argv) {
    const int msgs = argc > 1 ? std::atoi(argv[1]) : 400;

    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-wire"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    std::printf("transcript=%d msgs, viewport 100x40\n\n", msgs);
    std::printf("%-22s %12s %12s %10s\n",
                "action", "frame bytes", "changed", "ms");

    // ── Theme move: every colour on screen changes ──────────────────
    //
    // The AVERAGE is not the complaint. "Not snappy ALWAYS" is a tail
    // problem: if one key in twenty costs 10x the rest, the mean stays
    // inside budget and the hand still feels a hitch. So collect every
    // sample and print the distribution.
    {
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        std::string prev = frame(m);
        std::vector<double> samples;
        std::size_t worst_changed = 0, total_bytes = 0;
        for (int i = 0; i < 200; ++i) {
            auto t0 = clk::now();
            m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
            std::string cur = frame(m);
            samples.push_back(
                std::chrono::duration<double, std::milli>(clk::now() - t0).count());
            worst_changed = std::max(worst_changed, changed_bytes(prev, cur));
            total_bytes = cur.size();
            prev = std::move(cur);
        }
        std::sort(samples.begin(), samples.end());
        const auto pct = [&](double p) {
            return samples[static_cast<std::size_t>(p * (samples.size() - 1))];
        };
        std::printf("%-22s %12zu %12zu %10.1f\n",
                    "theme browser  \u2193", total_bytes, worst_changed, pct(1.0));
        std::printf("    p50=%.1f  p90=%.1f  p99=%.1f  MAX=%.1f ms"
                    "   (spread %.0fx)\n",
                    pct(0.50), pct(0.90), pct(0.99), pct(1.0),
                    pct(0.50) > 0 ? pct(1.0) / pct(0.50) : 0.0);
    }

    // ── Settings list move: only the cursor row changes ─────────────────
    {
        Model m = seeded(msgs);
        m = app::update(std::move(m),
                        Msg{OpenSettingsList{settings::Category::General}}).first;
        std::string prev = frame(m);
        double worst_ms = 0;
        std::size_t worst_changed = 0, total_bytes = 0;
        for (int i = 0; i < 12; ++i) {
            auto t0 = clk::now();
            m = app::update(std::move(m), Msg{SettingsListMove{+1}}).first;
            std::string cur = frame(m);
            worst_ms = std::max(worst_ms,
                std::chrono::duration<double, std::milli>(clk::now() - t0).count());
            worst_changed = std::max(worst_changed, changed_bytes(prev, cur));
            total_bytes = cur.size();
            prev = std::move(cur);
        }
        std::printf("%-22s %12zu %12zu %10.1f\n",
                    "settings list  \u2193", total_bytes, worst_changed, worst_ms);
    }

    std::printf("\nA cursor move repaints a row; a theme move repaints every\n"
                "cell that carries colour. If `changed` is close to `frame\n"
                "bytes` for the theme row, the whole viewport is on the wire\n"
                "for each keypress \u2014 which is what a slow terminal feels.\n");
    return 0;
}
