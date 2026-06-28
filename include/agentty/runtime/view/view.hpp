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

// Key for the in-flight run's SETTLED sub-turn prefix node (the head
// sub-turns of a streaming autopilot turn that have already settled and
// can seal while the tail still streams). Encoded as kStrataPrefixKey -
// run_head_index so it is stable across frames yet never collides with a
// real run-start index (which is a small message index) or kStrataLiveKey.
// build_settled_run renders [run_head, live_run_start) for this key.
inline constexpr std::uint64_t kStrataPrefixKey = ~std::uint64_t{0} - 1;

[[nodiscard]] std::vector<maya::strata::NodeRef> strata_nodes(const Model& m);
[[nodiscard]] maya::Element strata_build(const Model& m, std::uint64_t key);

} // namespace agentty::ui
