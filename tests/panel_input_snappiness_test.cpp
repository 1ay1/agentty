// Panel input snappiness + burst correctness.
//
// Two questions, both about what happens when keys arrive FASTER than a
// frame:
//
//   1. LATENCY — does one keypress fit inside a key-repeat slot (~33 ms at
//      30/s) on every panel, not just the theme browser?
//
//   2. CORRECTNESS under burst — a fast terminal delivers a whole key-repeat
//      run in ONE read(), and maya dispatches every event before painting
//      once. So N arrows must land on exactly the same state as N separate
//      frames would. If they don't, that's the "input race" class: a
//      coalesced burst that ends somewhere a single-stepped run wouldn't.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/runtime/panel/form_keys.hpp"

#include <maya/core/render_context.hpp>
#include <maya/app/inline.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;
using clk = std::chrono::steady_clock;
namespace pn = agentty::ui::panel;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); \
    std::printf("\n"); ++g_fail; } } while (0)

namespace {

Model seeded(int msgs) {
    Model m;
    for (int i = 0; i < msgs; ++i) {
        Message msg;
        msg.role = (i % 2) ? Role::Assistant : Role::User;
        msg.text = "## Heading " + std::to_string(i) +
            "\n\nProse with `code` and *emphasis*, long enough to wrap across "
            "a couple of lines.\n\n```cpp\nint f" + std::to_string(i) +
            "() { return 42; }\n```\n";
        m.d.current.messages.push_back(std::move(msg));
    }
    app::detail::rehydrate_frozen(m);
    return m;
}

void paint(const Model& m) {
    maya::RenderContext ctx{100, 40, maya::render_generation(), true};
    maya::RenderContextGuard g(ctx);
    auto el = ui::view(m);
    (void)maya::render_to_string(el, 100);
}

// Cost of ONE key: reduce + the paint that follows the batch.
double key_ms(Model& m, const Msg& key) {
    auto t0 = clk::now();
    m = app::update(std::move(m), Msg{key}).first;
    paint(m);
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

const char* verdict(double ms, double budget) {
    return ms <= budget ? "snappy" : "SLOW";
}

}  // namespace

int main() {
    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-snap"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    constexpr double kBudget = 33.3;   // one key-repeat slot at 30/s
    const int msgs = 800;              // a long session
    std::printf("transcript=%d msgs   budget=%.1f ms/key (30/s key repeat)\n\n",
                msgs, kBudget);

    // ── 1. Per-panel latency ────────────────────────────────────────────
    std::printf("%-26s %10s  %s\n", "panel / action", "ms/key", "verdict");

    {   // Theme browser: the one with 615 rows and a live re-seal per move.
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        for (int i = 0; i < 5; ++i) (void)key_ms(m, Msg{AppearanceThemeMove{+1}});
        double worst = 0;
        for (int i = 0; i < 40; ++i)
            worst = std::max(worst, key_ms(m, Msg{AppearanceThemeMove{+1}}));
        std::printf("%-26s %10.2f  %s\n", "theme browser  ↓", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "theme browser %.1f ms > %.1f budget", worst, kBudget);
    }

    {   // Typing in the theme filter: re-filters 615 names per keystroke.
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        double worst = 0;
        for (const char* c : {"d","r","a","c","u","l","a"}) {
            AppearanceThemeQuery q; q.text = c;
            worst = std::max(worst, key_ms(m, Msg{q}));
        }
        std::printf("%-26s %10.2f  %s\n", "theme filter   type", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "theme filter %.1f ms > %.1f budget", worst, kBudget);
    }

    {   // The appearance FORM itself (row navigation, no browser).
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        double worst = 0;
        for (int i = 0; i < 20; ++i) {
            AppearanceKey k;
            k.action = form::keys::Action{form::keys::Intent::MoveNext, 0};
            worst = std::max(worst, key_ms(m, Msg{k}));
        }
        std::printf("%-26s %10.2f  %s\n", "appearance form \u2193", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "appearance form %.1f ms > %.1f budget", worst, kBudget);
    }

    // The other panels a user holds an arrow in. Same budget, same reason:
    // every one of them repaints the transcript behind it, so a slow one
    // would show up here rather than in a bug report.
    {
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenPalette{}}).first;
        double worst = 0;
        for (int i = 0; i < 20; ++i)
            worst = std::max(worst, key_ms(m, Msg{PaletteMove{+1}}));
        std::printf("%-26s %10.2f  %s\n", "palette        \u2193", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "palette %.1f ms > %.1f budget", worst, kBudget);
    }
    {
        Model m = seeded(msgs);
        m = app::update(std::move(m), Msg{OpenThreadList{}}).first;
        double worst = 0;
        for (int i = 0; i < 20; ++i)
            worst = std::max(worst, key_ms(m, Msg{ThreadListMove{+1}}));
        std::printf("%-26s %10.2f  %s\n", "thread list    \u2193", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "thread list %.1f ms > %.1f budget", worst, kBudget);
    }
    {
        Model m = seeded(msgs);
        m = app::update(std::move(m),
                        Msg{OpenSettingsList{settings::Category::General}}).first;
        double worst = 0;
        for (int i = 0; i < 20; ++i)
            worst = std::max(worst, key_ms(m, Msg{SettingsListMove{+1}}));
        std::printf("%-26s %10.2f  %s\n", "settings list  \u2193", worst, verdict(worst, kBudget));
        CHECK(worst <= kBudget, "settings list %.1f ms > %.1f budget", worst, kBudget);
    }

    // ── 2. Burst == single-step (the race check) ────────────────────────
    //
    // A fast terminal delivers a key-repeat run in ONE read, and maya
    // dispatches every event before painting once. So 20 arrows delivered as
    // a burst must land exactly where 20 arrows delivered one-per-frame land.
    // Anything else means the landing spot depends on terminal timing.
    std::printf("\nburst vs single-step (20 \u2193):\n");
    {
        const int kN = 20;

        Model burst = seeded(msgs);
        burst = app::update(std::move(burst), Msg{OpenAppearance{}}).first;
        burst = app::update(std::move(burst), Msg{AppearancePickTheme{}}).first;
        for (int i = 0; i < kN; ++i)   // all events, THEN one paint
            burst = app::update(std::move(burst), Msg{AppearanceThemeMove{+1}}).first;
        paint(burst);

        Model step = seeded(msgs);
        step = app::update(std::move(step), Msg{OpenAppearance{}}).first;
        step = app::update(std::move(step), Msg{AppearancePickTheme{}}).first;
        for (int i = 0; i < kN; ++i) { // paint between every event
            step = app::update(std::move(step), Msg{AppearanceThemeMove{+1}}).first;
            paint(step);
        }

        const auto* ob = burst.ui.panel.get<pn::Appearance>();
        const auto* os = step.ui.panel.get<pn::Appearance>();
        std::printf("  index  burst=%d  stepped=%d\n",
                    ob ? ob->pane.picker.picker.index() : -1,
                    os ? os->pane.picker.picker.index() : -1);
        std::printf("  theme  burst=%s  stepped=%s\n",
                    burst.d.ui.theme.c_str(), step.d.ui.theme.c_str());
        CHECK(ob && os && ob->pane.picker.picker.index() == os->pane.picker.picker.index(),
              "burst landed on a different row than single-stepping");
        CHECK(burst.d.ui.theme == step.d.ui.theme,
              "burst landed on a different THEME than single-stepping");
    }

    // A wrap must also be timing-independent: 615 themes, walk past the end.
    {
        Model a = seeded(8), b = seeded(8);
        for (Model* p : {&a, &b}) {
            *p = app::update(std::move(*p), Msg{OpenAppearance{}}).first;
            *p = app::update(std::move(*p), Msg{AppearancePickTheme{}}).first;
        }
        const int kWrap = 620;   // past the end of the list
        for (int i = 0; i < kWrap; ++i)
            a = app::update(std::move(a), Msg{AppearanceThemeMove{+1}}).first;
        for (int i = 0; i < kWrap; ++i) {
            b = app::update(std::move(b), Msg{AppearanceThemeMove{+1}}).first;
            paint(b);
        }
        const auto* oa = a.ui.panel.get<pn::Appearance>();
        const auto* ob = b.ui.panel.get<pn::Appearance>();
        std::printf("  wrap(%d) burst=%d stepped=%d\n", kWrap,
                    oa ? oa->pane.picker.picker.index() : -1, ob ? ob->pane.picker.picker.index() : -1);
        CHECK(oa && ob && oa->pane.picker.picker.index() == ob->pane.picker.picker.index(),
              "wrapping past the end depends on paint timing");
        CHECK(a.d.ui.theme == b.d.ui.theme, "wrap landed on a different theme");
    }

    // ── 3. The routing race: a batch that CHANGES which panel owns keys ──
    //
    // This is the one a reducer-level test cannot see. maya routes each key
    // through the subscription built from the CURRENT model, and a fast
    // terminal delivers "Enter ↓ ↓ ↓" in one read. If the loop reused the
    // pre-Enter subscription for the arrows, they would route to the form
    // underneath (moving its cursor) instead of the browser that Enter just
    // opened — a keypress that visibly does nothing.
    //
    // maya re-fetches the sub after every drained message for exactly this
    // reason (app.hpp: "Re-route BETWEEN events, not just between reads").
    // Model that here: the panel state after the opening message must be
    // what the following messages are interpreted against.
    std::printf("\nrouting across a panel change (open + 3 \u2193 in one batch):\n");
    {
        Model m = seeded(64);
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;

        // Enter on the Theme row opens the browser — the panel-changing
        // event. Everything after it in the same read must land in the
        // BROWSER, not the form.
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        const auto* opened = m.ui.panel.get<pn::Appearance>();
        CHECK(opened && opened->pane.picking,
              "Enter did not open the browser");
        const int form_cursor_before = opened ? opened->pane.form.cursor : -1;

        for (int i = 0; i < 3; ++i)
            m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;

        const auto* o = m.ui.panel.get<pn::Appearance>();
        std::printf("  browser index=%d   form cursor %d\u2192%d\n",
                    o ? o->pane.picker.picker.index() : -1,
                    form_cursor_before, o ? o->pane.form.cursor : -1);
        CHECK(o && o->pane.picker.picker.index() == 3,
              "arrows after the opening key did not reach the browser");
        CHECK(o && o->pane.form.cursor == form_cursor_before,
              "arrows leaked into the form behind the browser");
    }

    std::printf("\n%s\n", g_fail == 0 ? "panel_input_snappiness: OK"
                                      : "panel_input_snappiness: FAILURES");
    return g_fail == 0 ? 0 : 1;
}
