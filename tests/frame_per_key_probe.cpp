// Frame-per-key fidelity: does every arrow you press produce a frame that
// SHOWS that arrow's result?
//
// Reported symptom: "I keep pressing down, suddenly one key doesn't move the
// row, then the next one moves twice." That is NOT a dropped key — a dropped
// key could never later move two rows. The state is right; the intermediate
// FRAME was never painted.
//
// Mechanism (maya app.hpp): a fast terminal delivers several arrows in ONE
// read(). The loop reduces each event, but only sets `needs_render = true`
// and paints ONCE for the whole batch. So the row the user passed through is
// computed and immediately overwritten before it ever reaches the terminal.
//
// This models the loop's batching exactly and counts DISTINCT rows the user
// would actually see, vs keys pressed.
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/panel/appearance.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;

int main(int argc, char** argv) {
    // How many arrows arrive in one read(). 1 = one key per frame (slow
    // typing); >1 = key-repeat outrunning the frame rate.
    const int batch  = argc > 1 ? std::atoi(argv[1]) : 3;
    const int total  = argc > 2 ? std::atoi(argv[2]) : 12;

    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-frames"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    Model m;
    for (int i = 0; i < 40; ++i) {
        Message msg;
        msg.role = (i % 2) ? Role::Assistant : Role::User;
        msg.text = "message " + std::to_string(i);
        m.d.current.messages.push_back(std::move(msg));
    }
    app::detail::rehydrate_frozen(m);
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;

    const auto row = [&] {
        const auto* o = m.ui.panel.get<pn::Appearance>();
        return o ? o->pane.picker.index : -1;
    };

    std::printf("keys arriving %d-per-read, %d keys total\n\n", batch, total);
    std::printf("  read#   keys         row AFTER paint   rows the user SAW\n");

    // FIXED LOOP (maya app.hpp): a read is consumed only up to and
    // including the first NAVIGATION key; the rest is pushed back and
    // delivered next iteration, after this frame is painted. So a batch of
    // arrows becomes one arrow per frame, while a paste (non-navigation)
    // still drains in one go.
    const bool nav_breaks_batch = std::getenv("OLD_BATCHING") == nullptr;

    std::vector<int> seen;          // one entry per PAINT, not per key
    int pressed = 0;
    std::vector<int> deferred;      // arrows held over from the last read
    for (int r = 0; pressed < total || !deferred.empty(); ++r) {
        int n = 0;
        if (!deferred.empty()) {
            n = static_cast<int>(deferred.size());
            deferred.clear();
        } else {
            n = std::min(batch, total - pressed);
            pressed += n;
        }
        // Reduce events from this read, stopping after the first nav key.
        int applied = 0;
        for (int k = 0; k < n; ++k) {
            m = app::update(std::move(m), Msg{AppearanceThemeMove{+1}}).first;
            ++applied;
            if (nav_breaks_batch && applied < n) {
                deferred.assign(static_cast<std::size_t>(n - applied), 1);
                break;
            }
        }
        // ...then paint ONCE for what was consumed.
        seen.push_back(row());
        std::printf("  %-6d  %-11d  %-16d  %d\n", r + 1, applied, row(), row());
    }

    std::printf("\n  keys pressed         : %d\n", total);
    std::printf("  frames painted       : %zu\n", seen.size());
    std::printf("  rows the user SAW    : %zu of %d\n", seen.size(), total);

    if (seen.size() < static_cast<std::size_t>(total)) {
        std::printf("\n  → %zu row(s) were computed but NEVER SHOWN.\n",
                    total - seen.size());
        std::printf("    Felt as: a key that \"does nothing\", then the next "
                    "one \"moves twice\".\n");
        // Name the skipped rows, which is exactly what the user watches jump.
        std::printf("    Visible sequence: ");
        for (std::size_t i = 0; i < seen.size(); ++i)
            std::printf("%d%s", seen[i], i + 1 < seen.size() ? " → " : "\n");
        std::printf("    Expected        : ");
        for (int i = 1; i <= total; ++i)
            std::printf("%d%s", i, i < total ? " → " : "\n");
    } else {
        std::printf("\n  → every keypress got its own frame.\n");
    }
    return 0;
}
