#pragma once
// agentty::ui::view — top-level composition.
//
// Assembles the per-panel views into the root Element.  All overlay logic
// (modals, pickers) lives here so per-panel files stay focused.

#include <cstdint>
#include <vector>

#include <maya/maya.hpp>
#include <maya/render/strata.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

[[nodiscard]] maya::Element view(const Model& m);

// ── Strata (depositional inline rendering) ───────────────────────────
// The host hands maya a flat node list + a lazy builder instead of one
// monolithic view(). Settled turns (m.ui.frozen) are terminal nodes maya
// seals into native scrollback itself; the live tail + chrome + overlay is
// one non-terminal LIVE node. No host commit_scrollback, no frozen-height
// accounting — maya owns the whole scrollback discipline.
//
// Stable key for the always-live bottom chrome node (rendered last, never
// sealed). Distinct from any frozen index.
inline constexpr std::uint64_t kStrataLiveKey = ~std::uint64_t{0};

[[nodiscard]] std::vector<maya::strata::NodeRef> strata_nodes(const Model& m);
[[nodiscard]] maya::Element strata_build(const Model& m, std::uint64_t key);

} // namespace agentty::ui
