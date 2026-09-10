#pragma once
// Stats viewer — a tabbed, read-only window onto what the session did.
//
// Open it from the command palette (Ctrl+K → "Stats"). Tab/Shift-Tab (or
// ←/→) switch tabs, Esc closes. Nothing here mutates anything: the panel
// is a projection of the transcript, so there is no state to get wrong
// and no confirmation to design.
//
// ── What lives where ─────────────────────────────────────────────────────
//
//   domain/stats/facts.hpp   the counters + the incremental fold
//   domain/stats/unit.hpp    the ONE number→text function
//   domain/stats/metric.hpp  the row vocabulary
//   domain/stats/tabs.hpp    kTabs — THE SSOT for order/titles/availability
//   view/panels/stats.cpp    the panel: table-driven, no per-tab branches
//
// Adding a tab is an enumerator, a row in kTabs, and an extractor. It
// touches no view code, no key handling and no panel code.
//
// ── Why the projection lives on the open panel ───────────────────────────
//
// It is a CACHE, not a second source of truth: every number in it is
// derived from the transcript and can be rebuilt from it at any time. It
// sits here because its lifetime is exactly the panel's — opening the
// panel starts folding, closing it frees everything — and because a
// projection that outlived the panel would be a background cost paid by
// users who never open it.
//
// `mutable` because the view refreshes it during render, which is a const
// operation on the model by construction: refresh() only ever recomputes
// what the transcript already says.

#include "agentty/domain/stats/facts.hpp"
#include "agentty/domain/stats/tabs.hpp"

namespace agentty::stats_panel {

struct Open {
    // Which tab is live. Mutable because the view corrects it when the
    // selected tab becomes unavailable (the last tool call was rewound
    // away) — landing on a tab that is no longer drawn would show an
    // empty panel with no way to understand why.
    mutable stats::Tab tab = stats::Tab::Session;

    // The incremental fold. Amortised O(1) per frame: a settled thread
    // folds nothing, a streaming one folds exactly the live tail. See
    // domain/stats/facts.hpp for the cursor and the epoch guard.
    mutable stats::Projection projection;
};

}  // namespace agentty::stats_panel
