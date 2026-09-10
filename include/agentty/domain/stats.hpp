#pragma once
// agentty::stats — DERIVED session statistics.
//
// ── The design rule ──────────────────────────────────────────────────────
//
// Everything here is COMPUTED FROM THE TRANSCRIPT, never accumulated
// alongside it. There is no counter to increment, no tally to keep in sync,
// no "stats got out of step with reality" bug available to write. Feed the
// same messages in and you get the same numbers out.
//
// That is not a stylistic preference — it is the correctness argument. A
// stats view whose job is to answer "is Smart Mode actually working?" is
// worthless if it can drift from what the router did, and a parallel counter
// is exactly how that drift happens: an early return that skips an
// increment, a retry that double-counts, a path that forgets. Deriving from
// the record the wire already produced makes the answer true by
// construction.
//
// What makes this possible: every assistant Message already carries its own
// provenance (`served_model`, `served_role`) because the turn header needs
// it. Those fields are written at dispatch by the same value the request
// used, so a role tally over the transcript IS the routing history — not a
// reconstruction of it.
//
// ── What is deliberately NOT here ────────────────────────────────────────
//
// Token and cost totals. Session tracks tokens_in/tokens_out for the LAST
// turn only (they are replaced per turn, not accumulated), and no per-message
// token record exists. Showing a session total would mean estimating, and an
// estimated number in a panel whose purpose is verification is worse than no
// number: the user cannot tell which digits to trust. Turn counts are exact,
// so turn counts are what this reports.

#include <cstddef>
#include <string>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/domain/smart_mode.hpp"

namespace agentty::stats {

// ── One row of a tally ───────────────────────────────────────────────────
// A label, a count, and the share it represents. `share` is carried rather
// than recomputed at the view because two views (the bar and the legend)
// would otherwise divide independently and could disagree in the last digit.
struct Row {
    std::string label;
    std::size_t count = 0;
    double      share = 0.0;   // 0..1 of the tally's total
};

// ── Smart Mode ───────────────────────────────────────────────────────────
//
// The question this answers is the one a user actually asks: "is my
// expensive model still doing all the work?"
struct SmartStats {
    // Assistant turns that carry routing provenance at all. A turn with no
    // served_role ran with Smart Mode OFF; counting it as "strategic" would
    // overstate the flagship's share, and dropping it silently would
    // understate the denominator. It gets its own bucket.
    std::size_t routed_turns   = 0;   // served_role present
    std::size_t unrouted_turns = 0;   // Smart Mode was off for these
    std::size_t total_turns    = 0;   // routed + unrouted

    // Per-role tally over the routed turns, in ladder order (Strategic,
    // Implementation, Utility) so the shape reads consistently frame to
    // frame rather than reordering as counts change.
    std::vector<Row> by_role;

    // Per-model tally over the routed turns, strongest share first. This is
    // the row a user checks against their provider's billing page.
    std::vector<Row> by_model;

    // The headline: share of routed turns that did NOT run on the Strategic
    // role. This is the number that was structurally 0.0 before the main
    // turn was routed by complexity, and it is what "is it working?" means
    // in one figure.
    double delegated_share = 0.0;

    // True when there is nothing to report yet. The view shows an
    // explanatory empty state rather than a wall of zeroes, which read as a
    // failure when they only mean "no turns yet".
    [[nodiscard]] bool empty() const noexcept { return total_turns == 0; }
};

// Compute the Smart Mode tallies for a transcript.
//
// Pure: reads messages, allocates its result, touches nothing else. Safe to
// call every frame — but see the panel's cache note, because it is O(turns)
// and the view has a per-frame budget.
[[nodiscard]] SmartStats smart_stats(const std::vector<Message>& messages);

// ── Tabs ─────────────────────────────────────────────────────────────────
//
// The viewer is built as a tabbed surface from the start even though Smart
// Mode is the only tab today. A second tab must be a new enumerator plus a
// new arm — not a refactor of a single-purpose panel into a general one,
// which is the migration that never gets done.
//
// Order here IS tab order; the enum is the SSOT for both.
enum class Tab : std::uint8_t {
    Smart,
};

inline constexpr int kTabCount = 1;

[[nodiscard]] constexpr std::string_view tab_title(Tab t) noexcept {
    switch (t) {
        case Tab::Smart: return "Smart Mode";
    }
    return "Smart Mode";
}

// One-line description under the tab strip: what this tab is for, in the
// user's terms. Lives beside the title so adding a tab makes both
// obligations visible at the same site.
[[nodiscard]] constexpr std::string_view tab_subtitle(Tab t) noexcept {
    switch (t) {
        case Tab::Smart:
            return "which model actually served each turn";
    }
    return "";
}

// Step the tab selection, wrapping. Total on the enum, so a new tab joins
// the cycle with no key-handling change.
[[nodiscard]] constexpr Tab tab_step(Tab t, int delta) noexcept {
    const int n = kTabCount;
    int i = static_cast<int>(t) + delta;
    while (i < 0) i += n;
    return static_cast<Tab>(i % n);
}

} // namespace agentty::stats
