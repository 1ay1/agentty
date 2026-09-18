// Deterministic repro, layer 3: AGENTTY's view + frozen ledger.
//
// The two maya-layer repros both PASS: a theme swap is detected, the cells
// change, style ids do not collide, and the emitted bytes carry the new ink.
// So maya renders correctly when handed a correctly-rebuilt tree.
//
// That moves the suspicion back to agentty: the FROZEN LEDGER holds prebuilt
// Element values. If a theme change does not rebuild those Elements, maya is
// handed a tree that still carries the old palette — and maya will faithfully
// render exactly what it was given. Every maya-layer test would still pass.
//
// This drives the real reducer path (move_highlight -> restyle_sealed_turns ->
// rehydrate_frozen) and asserts the property that matters end to end:
//
//   after one simulated arrow key, the FROZEN ledger's rendered bytes must
//   carry the newly selected scheme's ink.
//
// It also asserts the one the user actually reported: TWO consecutive arrow
// keys must produce TWO different palettes on screen — not one change for
// every two presses.

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/view/view.hpp"
#include "agentty/domain/ui_theme.hpp"
#include "agentty/io/persistence.hpp"

#include <maya/maya.hpp>
#include <maya/app/inline.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace agentty;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

constexpr int kWidth = 100;

// What the FROZEN ledger would put on the wire, colours included.
std::string frozen_bytes(const Model& m) {
    std::string out;
    for (const auto& blk : m.ui.frozen.elements())
        out += maya::render_to_string_ansi(blk, kWidth);
    return out;
}

// What the WHOLE view would put on the wire.
std::string view_bytes(const Model& m) {
    return maya::render_to_string_ansi(ui::view(m), kWidth);
}

} // namespace

int main(int argc, char** argv) {
    const int msgs = argc > 1 ? std::atoi(argv[1]) : 200;
    // Optional: a REAL thread file. Synthetic messages do not reproduce the
    // frozen ledger's real block shapes (tool cards, code blocks, folds),
    // and the whole bug lives in how those blocks repaint.
    const char* thread_path = argc > 2 ? argv[2] : nullptr;

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

    std::printf("=== repro 3: agentty view + frozen ledger ===\n\n");

    Model m;
    if (thread_path) {
        // A thread id, not a path: the store speaks the .jsonl log format
        // (load_thread_file is the LEGACY whole-document .json reader, which
        // silently returns nothing for a log file).
        auto t = persistence::load_thread_by_id(ThreadId{thread_path});
        if (t) {
            m.d.current = std::move(*t);
            std::printf("loaded thread %s: %zu messages\n\n",
                        thread_path, m.d.current.messages.size());
        } else {
            std::printf("could not load thread %s — falling back to synthetic\n\n",
                        thread_path);
        }
    }
    if (m.d.current.messages.empty()) {
        for (int i = 0; i < msgs; ++i) {
            Message msg;
            msg.role = (i % 2) ? Role::Assistant : Role::User;
            msg.text = "## Heading " + std::to_string(i) +
                "\n\nProse with `code` and *emphasis* that wraps across lines.\n\n"
                "```cpp\nint f" + std::to_string(i) + "() { return 42; }\n```\n";
            m.d.current.messages.push_back(std::move(msg));
        }
    }

    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;

    // Pin the colour TIER.
    //
    // resolve() falls back to `native` for every named scheme when the tier
    // is too low to paint one — and detect_tier() reports the lowest tier
    // headlessly, because stdout is a pipe. Without this the probe resolves
    // EVERY theme to the same palette and "the ledger did not change" is a
    // property of the harness, not of the code under test. Forcing TrueColor
    // is what a real terminal session already has.
    m.d.ui.tier = ui_prefs::ColorTier::TrueColor;

    app::detail::rehydrate_frozen(m);

    std::printf("transcript=%d msgs, frozen_through=%zu, blocks=%zu\n\n",
                msgs, m.ui.frozen_through, m.ui.frozen.elements().size());

    // One "frame": what a real loop does — view() publishes + builds, and we
    // capture what would go to the wire.
    auto frame = [&](std::string& frozen, std::string& whole) {
        whole  = view_bytes(m);     // publishes the theme as a real frame does
        frozen = frozen_bytes(m);
    };

    // ── One arrow key must change the screen ─────────────────────────────
    {
        std::printf("a single arrow key changes what is on screen\n");
        std::string f0, v0, f1, v1;
        frame(f0, v0);
        const std::string t0 = m.d.ui.theme;

        m = app::update(std::move(m), Msg{AppearanceThemeMove{1}}).first;
        frame(f1, v1);
        const std::string t1 = m.d.ui.theme;

        std::printf("    theme: '%s' -> '%s'\n", t0.c_str(), t1.c_str());
        // Distinguish "the reducer never picked a real scheme" from "it did
        // but the ledger did not rebuild". resolve() falls back to native
        // whenever the colour TIER is too low to paint a named scheme, so a
        // headless run can silently resolve everything to the same palette —
        // which would look exactly like the bug without being it.
        {
            const auto r0 = ui_prefs::resolve(m.d.ui, /*tty=*/true);
            std::printf("    resolved tier=%d  scheme=%s  ink=%02x%02x%02x\n",
                        static_cast<int>(r0.tier),
                        (r0.theme == &maya::theme::native) ? "native(FALLBACK)"
                                                           : "named",
                        r0.theme->text.r(), r0.theme->text.g(), r0.theme->text.b());
        }
        check(t0 != t1,  "the model's theme actually changed");
        check(v0 != v1,  "the rendered VIEW differs after one press");
        check(f0 != f1,  "the FROZEN ledger differs after one press");
        std::printf("\n");
    }

    // ── THE REPORT: two presses, two visibly different screens ───────────
    // "It takes two arrow movements to see the theme change" means press N
    // and press N+1 render identically. Assert they do not.
    {
        std::printf("consecutive presses each change the screen\n");
        int unchanged = 0;
        std::string prev_f, prev_v;
        frame(prev_f, prev_v);

        for (int k = 0; k < 8; ++k) {
            m = app::update(std::move(m), Msg{AppearanceThemeMove{1}}).first;
            std::string f, v;
            frame(f, v);
            if (f == prev_f) {
                ++unchanged;
                std::printf("    press %d: frozen ledger IDENTICAL to previous\n",
                            k + 1);
            }
            prev_f = std::move(f);
            prev_v = std::move(v);
        }
        std::printf("    presses that changed nothing on screen: %d / 8\n",
                    unchanged);
        check(unchanged == 0, "every press repaints (no two-press lag)");
        std::printf("\n");
    }

    if (failures == 0) {
        std::printf("PASS — no lag reproduced in the reducer/view layer\n");
        return 0;
    }
    std::printf("FAILED: %d check(s) — bug reproduced here\n", failures);
    return 1;
}
