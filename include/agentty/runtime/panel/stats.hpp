#pragma once
// Stats viewer — a tabbed, read-only window onto what the session actually
// did.
//
// Open it from the command palette (Ctrl+K → "Stats"). Tab/Shift-Tab (or
// ←/→) switch tabs, Esc closes. Nothing here mutates anything: the panel is
// a projection of the transcript, so there is no state to get wrong and no
// confirmation to design.
//
// ── Why tabbed from day one ──────────────────────────────────────────────
//
// Smart Mode is the only tab today. It is still built as a tab set, because
// the alternative is a single-purpose panel that someone later has to
// generalise — and that refactor is the one that never happens, so the
// second stat lands as a second panel instead, with its own key handling and
// its own chrome. The tab enumeration lives in domain/stats.hpp
// (stats::Tab), which is the SSOT for order, titles and subtitles; adding a
// tab is an enumerator plus a render arm, and the compiler names both.
//
// ── Why the stats are cached here ────────────────────────────────────────
//
// stats::smart_stats() is pure and O(turns). The view builds every frame, and
// on a long thread an O(turns) walk plus a map and a sort per frame is real
// work on the same thread as the streaming reveal — exactly the class of
// cost that regressed maya's glide (see the COST CONTRACT in
// maya/include/maya/element/builder.hpp).
//
// So the panel holds the computed result and a `stamp` of the input it was
// computed from. The view compares stamps and recomputes only when the
// transcript actually changed. That keeps the panel a pure projection —
// the cache is an optimisation, never a second source of truth: throw it
// away and you get the same numbers back.

#include <cstddef>

#include "agentty/domain/stats.hpp"

namespace agentty::stats_panel {

// The cache-validity stamp.
//
// Message count is the whole signal, and it is sufficient rather than merely
// convenient: a served turn's provenance is stamped once at dispatch and is
// immutable afterwards, so the tally can only change when a message is
// APPENDED. Streaming text growing on the in-flight turn does not affect any
// number here (that turn has no served_model until it dispatches), which is
// what makes a cheap stamp correct instead of a heuristic.
//
// Thread switches also change the count in practice, and the thread id is
// carried anyway so an unlucky same-length thread cannot alias.
struct Stamp {
    std::size_t message_count = 0;
    std::string thread_id;

    [[nodiscard]] bool operator==(const Stamp&) const = default;
};

struct Open {
    stats::Tab tab = stats::Tab::Smart;

    // Cached projection + the input it came from. Mutable because the view
    // refreshes it lazily during render; see the header note on why that is
    // an optimisation and not shared state.
    mutable stats::SmartStats smart{};
    mutable Stamp             stamp{};
};

}  // namespace agentty::stats_panel
