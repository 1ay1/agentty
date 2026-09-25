// theme_alternating_key_test — the gesture that actually reproduces it.
//
// THE REPORT. Pressing Down then Up (or Up then Down) does not update the
// screen. Pressing the SAME direction twice does. Enter then commits a
// theme that was never shown live.
//
// WHY EVERY EXISTING PROBE MISSED IT. theme_wire_cost_probe, theme_lag_repro
// and friends all render through `maya::render_to_string(el, w)`, which
// builds a FRESH StylePool and a fresh component cache per call. That throws
// away the one thing the bug lives in: state carried BETWEEN frames. A
// cache that is empty every frame can never serve a stale entry, so those
// probes were structurally incapable of failing on this.
//
// This test keeps ONE renderer across frames — the way the real app does —
// and drives the exact alternating gesture from the report.
//
// WHAT ALTERNATING VS REPEATING TELLS US. It is the signature of a memo
// keyed on something that does not include the theme. Down→Up returns the
// cursor to a row that was already built and cached under the PREVIOUS
// palette, so the cache answers and the new colours never reach the wire.
// Down→Down lands on a row with no entry yet, so it is built fresh under the
// new palette and appears to work. Same keystroke count, opposite outcome —
// which is exactly what a content-keyed cache with no theme in the key does.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <maya/maya.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/io/persistence.hpp"

using namespace agentty;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

constexpr int kW = 100;
constexpr int kH = 40;

// A renderer whose pool and component cache PERSIST across frames, which is
// what `maya::run` does and what render_to_string does not.
class Screen {
public:
    Screen() : canvas_(kW, kH, &pool_) {}

    // Render one frame and return the cells as text+colour, so a comparison
    // sees a palette change even when the glyphs are identical.
    std::string frame(const Model& m) {
        maya::RenderContext ctx{kW, kH, maya::render_generation(), true};
        maya::RenderContextGuard g(ctx);

        // view() publishes the palette (ui_prefs::publish_theme) as a side
        // effect of building, exactly as the real host does.
        auto el = ui::view(m);

        // The real loop's order: the pool learns of a theme swap, then the
        // tree is rendered into the same canvas as last frame.
        const bool swapped = pool_.retheme();
        canvas_.clear();
        std::vector<maya::layout::LayoutNode> nodes;
        maya::render_tree(el, canvas_, pool_, maya::theme::live(), nodes,
                          /*auto_height=*/true);
        last_retheme_ = swapped;
        last_epoch_   = maya::theme::live_epoch();
        last_live_    = &maya::theme::live();

        // Serialize cell styles, not just glyphs: the whole question is
        // whether the COLOURS moved.
        std::string out;
        out.reserve(static_cast<std::size_t>(kW) * kH * 4);
        for (int y = 0; y < canvas_.height(); ++y) {
            for (int x = 0; x < canvas_.width(); ++x) {
                const auto c = canvas_.get(x, y);
                out += static_cast<char>('a' + (c.style_id % 26));
                out += static_cast<char>(
                    c.character < 128 ? static_cast<char>(c.character) : '?');
            }
        }
        return out;
    }

    [[nodiscard]] bool retheme_fired() const noexcept { return last_retheme_; }
    [[nodiscard]] unsigned epoch() const noexcept { return last_epoch_; }
    [[nodiscard]] const void* live() const noexcept { return last_live_; }

private:
    maya::StylePool pool_;
    maya::Canvas    canvas_;
    bool            last_retheme_ = false;
    unsigned        last_epoch_   = 0;
    const void*     last_live_    = nullptr;
};

Model seeded() {
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
    for (int i = 0; i < 40; ++i) {
        Message msg;
        msg.role = (i % 2 == 0) ? Role::User : Role::Assistant;
        msg.text = "message " + std::to_string(i)
                 + " with some **bold** text and `code` in it";
        m.d.current.messages.push_back(std::move(msg));
    }
    app::detail::rehydrate_frozen(m);
    return m;
}

} // namespace

int main() {
    // Force the tier BEFORE anything resolves a theme.
    //
    // Without this the test is vacuous and looks like a bug: under ctest
    // stdout is a pipe, detect_tier answers Mono, and ui_prefs::resolve's
    // `can_paint` gate collapses EVERY named scheme to native. Each frame
    // then legitimately renders identically, and the test reports a screen
    // that never changes -- while the real terminal, which is truecolor, is
    // fine. MAYA_COLOR is checked above every other signal (detect_tier
    // step 0), so this makes the harness ask the question it means to ask:
    // given a palette the terminal CAN paint, does a keystroke repaint?
#if defined(_WIN32)
    _putenv_s("MAYA_COLOR", "truecolor");
#else
    ::setenv("MAYA_COLOR", "truecolor", 1);
#endif

    std::printf("=== theme_alternating_key_test ===\n\n");
    std::printf("one renderer across frames (pool + component cache persist)\n\n");

    // ── 1. The reported gesture: alternating directions ─────────────────
    {
        Screen scr;
        Model m = seeded();
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        std::string prev = scr.frame(m);

        std::printf("alternating  \u2193 \u2191 \u2193 \u2191 \u2193 \u2191\n");
        int dead = 0;
        int gated = 0;
        std::uint64_t prev_hash = app::AgenttyApp::visual_hash(m);
        for (int i = 0; i < 6; ++i) {
            const int dir = (i % 2 == 0) ? +1 : -1;
            m = app::update(std::move(m), Msg{AppearanceThemeMove{dir}}).first;

            // THE GATE, which every other probe skips by calling view()
            // directly. maya renders only when this moves; a hash that
            // repeats means the frame is dropped no matter how different
            // view() would have been.
            const std::uint64_t h = app::AgenttyApp::visual_hash(m);
            const bool hash_moved = (h != prev_hash);
            prev_hash = h;

            std::string cur = scr.frame(m);
            const bool moved = (cur != prev);
            std::printf("    %-4s theme=%-22s hash=%s screen %s\n",
                        dir > 0 ? "down" : "up",
                        m.d.ui().theme.empty() ? "native" : m.d.ui().theme.c_str(),
                        hash_moved ? "moved " : "SAME  ",
                        moved ? "changed" : "DID NOT CHANGE");
            if (!moved) ++dead;
            if (!hash_moved) ++gated;
            prev = std::move(cur);
        }
        check(gated == 0, "the frame gate never eats an alternating press");
        check(dead == 0, "every alternating press repaints");
        std::printf("\n");
    }

    // ── 2. The control: the same direction twice ────────────────────────
    // The report says this one works. If it passes while the above fails,
    // the difference is the cache, not the reducer.
    {
        Screen scr;
        Model m = seeded();
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
        std::string prev = scr.frame(m);

        std::printf("repeating    \u2193 \u2193 \u2193 \u2193 \u2193 \u2193\n");
        int dead = 0;
        for (int i = 0; i < 6; ++i) {
            m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
            std::string cur = scr.frame(m);
            const bool moved = (cur != prev);
            std::printf("    down theme=%-22s screen %s\n",
                        m.d.ui().theme.empty() ? "native" : m.d.ui().theme.c_str(),
                        moved ? "changed" : "DID NOT CHANGE");
            if (!moved) ++dead;
            prev = std::move(cur);
        }
        check(dead == 0, "every repeated press repaints");
        std::printf("\n");
    }

    // ── 3. Returning to a row seen under the OLD palette ────────────────
    // The minimal shape of the bug: visit row N, move away, change the
    // palette by moving, come back. If a memo keyed without the theme is
    // serving row N, the frame that returns to it paints the old colours.
    {
        Screen scr;
        Model m = seeded();
        m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
        m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;

        scr.frame(m);
        const std::string theme_at_start = m.d.ui().theme;

        m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
        const std::string away_frame  = scr.frame(m);
        const std::string theme_away  = m.d.ui().theme;

        m = app::update(std::move(m), Msg{AppearanceThemeMove{-1}}).first;
        const std::string back_frame  = scr.frame(m);
        const std::string theme_back  = m.d.ui().theme;

        std::printf("leave a row and come back\n");
        std::printf("    start=%s  away=%s  back=%s\n",
                    theme_at_start.empty() ? "native" : theme_at_start.c_str(),
                    theme_away.empty()     ? "native" : theme_away.c_str(),
                    theme_back.empty()     ? "native" : theme_back.c_str());
        check(theme_back == theme_at_start,
              "the model returns to the starting theme");
        check(back_frame != away_frame,
              "the screen returns too (not stuck on the row we left)");
        std::printf("\n");
    }

    if (failures == 0) {
        std::printf("PASS -- alternating and repeating both repaint\n");
        return 0;
    }
    std::printf("FAILED: %d check(s) -- bug reproduced\n", failures);
    return 1;
}
