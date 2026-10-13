// SPDX-License-Identifier: Apache-2.0
//
// rehydrate_scrollback_test.cpp — a resumed thread leaves real history in
// the terminal's native scrollback, and the live canvas stays small.
//
// THE BUG THIS PINS. rehydrate_frozen() used ONE budget for two different
// questions: how much transcript to paint when resuming a thread, and how
// much to keep live for re-rendering. frozen.cpp justified the small
// window by saying older rows were safe because they "live on disk (recall
// via picker)" and were "committed to native scrollback when they
// overflowed live".
//
// Neither is true on a thread SWITCH. Nothing overflowed live -- the
// thread was just read off disk -- and reset_inline() emits \x1b[3J first,
// which wipes whatever scrollback was there. Measured on a real
// 3326-message thread: the switch emitted one 3J and then 174 newlines,
// and the canvas held 82 rows. Scrolling up showed nothing. The user had
// one turn of context and no way to see more.
//
// The fix separates the budgets: rehydrate seeds rehydrate_row_budget()
// (~10 viewports), and the existing post-paint trim walks it back down to
// frozen_row_budget() (~3 viewports). The trim is what makes this work --
// drop_front accrues each dropped block's paint-recorded height as
// ScrollbackDebt and harvest() mints a commit_scrollback token, so the
// rows physically scroll into the terminal's saved-lines instead of being
// discarded.
//
// Three properties, and the test fails if any regresses:
//   1. the seed is WIDER than the live budget (there is something to give
//      to scrollback at all)
//   2. the trim brings the live canvas back UNDER the live budget (resize
//      and Ctrl-L cost what they cost before)
//   3. the trim COMMITS the difference rather than dropping it (the rows
//      are scrollable, not lost)

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/update.hpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

using agentty::Model;
using agentty::Message;
using agentty::Role;

// A transcript long enough to blow past even the wide budget, so the walk
// and the trim both have real work. Bodies are multi-line so row counts
// are driven by content rather than chrome.
void build_long_thread(Model& m, int turns) {
    m.d.current.messages.clear();
    for (int i = 0; i < turns; ++i) {
        Message u;
        u.role = Role::User;
        u.text = "user turn " + std::to_string(i) + " asking for something";
        m.d.current.messages.push_back(std::move(u));

        Message a;
        a.role = Role::Assistant;
        a.text = "assistant reply " + std::to_string(i) + "\n"
                 "second line of the reply\n"
                 "third line of the reply\n"
                 "fourth line to give this turn some height\n";
        m.d.current.messages.push_back(std::move(a));
    }
}

}  // namespace

int rehydrate_scrollback_test_main() {
    std::printf("=== rehydrate_scrollback_test ===\n");

    namespace detail = agentty::app::detail;

    // 1. The two budgets are distinct, and the seed is the wider one.
    //    Checked at a fixed row count so the assertion does not depend on
    //    the terminal the test happens to run under.
    const std::size_t live = detail::frozen_row_budget(40);
    const std::size_t seed = detail::rehydrate_row_budget(40);
    std::printf("  live canvas budget: %zu rows,  rehydrate seed: %zu rows\n",
                live, seed);
    check(seed > live,
          "rehydrate seeds MORE than the live canvas keeps");
    check(seed >= live * 2,
          "the seed is wide enough to be worth committing (>= 2x)");

    // 1b. The reducers' budgets read the terminal size from the Model, which
    //     the host feeds as TerminalResized. So the answer is whatever the
    //     Msg said, never what the test's own tty happens to be.
    {
        Model sized;
        (void)agentty::app::update(sized, agentty::msg::MetaMsg{agentty::TerminalResized{120, 40}});
        check(sized.ui.term_cols == 120 && sized.ui.term_rows == 40,
              "TerminalResized lands in the Model");
        check(detail::frozen_row_budget(sized) == live,
              "the live budget follows the Model's rows");
        (void)agentty::app::update(sized, agentty::msg::MetaMsg{agentty::TerminalResized{0, 0}});
        check(sized.ui.term_rows == 40,
              "a 0x0 report (detached tty) keeps the last good size");
    }

    // 2. Seeding a long thread fills the canvas past the LIVE budget --
    //    i.e. there is something for the trim to hand to scrollback.
    Model m;
    build_long_thread(m, 400);
    detail::rehydrate_frozen(m);

    const std::size_t seeded_rows = m.ui.frozen.row_total();
    std::printf("  seeded canvas: %zu entries, %zu rows\n",
                m.ui.frozen.size(), seeded_rows);
    check(seeded_rows > live,
          "the seeded canvas exceeds the live budget (trim has work)");
    // NOTE: pending_rehydrate_trim is armed by the ThreadLoaded reducer,
    // not by rehydrate_frozen itself — this test calls the latter
    // directly, so the flag is not part of its contract.

    // 3. Stand in for the first paint.
    //
    //    The ledger's PROVABILITY GATE is the whole reason this works
    //    safely, and it is why the test has to do this: drop_front() drops
    //    only blocks whose height a real ledger-tagged paint has RECORDED,
    //    stopping at the first unrecorded one. A freshly sealed block has
    //    no recorded height, so dropping it would shed real wire rows
    //    while accruing zero debt -- an under-commit, the stranded-
    //    duplicate ghost. Hence agentty defers the trim to a post-paint
    //    Tick rather than running it inline after rehydrate.
    //
    //    Headless, nothing paints, so every block stays unrecorded and a
    //    trim here would correctly drop nothing. record_paint() is what
    //    the renderer calls per frame; calling it makes the prefix
    //    droppable exactly as the first real frame would.
    for (std::size_t k = 0; k < m.ui.frozen.size(); ++k)
        m.ui.frozen.record_paint(k, static_cast<int>(m.ui.frozen.block_rows(k)));

    // 4. The trim brings the canvas back under the live budget AND emits a
    //    commit rather than silently dropping. A none() Cmd here would mean
    //    the rows were discarded -- the exact failure this test exists for.
    const auto cmd = detail::trim_frozen_if_oversized(m);
    const std::size_t trimmed_rows = m.ui.frozen.row_total();
    std::printf("  after trim:    %zu entries, %zu rows\n",
                m.ui.frozen.size(), trimmed_rows);

    check(trimmed_rows < seeded_rows,
          "the trim SHRANK the canvas (rows left for scrollback)");
    check(!cmd.is_none(),
          "the trim COMMITS the dropped rows (scrollback, not /dev/null)");
    check(trimmed_rows <= live * 2,
          "the live canvas is bounded after the trim");

    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
