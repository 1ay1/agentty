#pragma once
// agentty::ui — the ONE definition of every inter-turn seam Element.
//
// The frozen builder (frozen.cpp / freeze_range) and the live-tail
// builder (conversation.cpp / build_live_tail) must produce
// byte-identical row sequences for the same input: at the freeze
// instant the live rows are simply re-labelled as frozen, and any
// 1-row shape delta shifts rows already committed to native terminal
// scrollback against the re-layout — the classic freeze-seam ghost.
//
// Historically each builder had its OWN copy of these Elements with a
// "MUST stay byte-identical to the other file" comment. That is a
// convention, not a guarantee — one drifted edit = one scrollback
// ghost. This header replaces the convention with a single definition
// site: both builders call the same functions, so the seam cannot
// diverge. (INLINE_SCROLLBACK.md pin #3 — divider symmetry — is now
// enforced by the linker, not by review.)

#include <cstddef>
#include <string>
#include <vector>

#include <maya/dsl.hpp>
#include <maya/element/element.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

// Inter-turn seam between every pair of adjacent turns — a blank row,
// the dim ─ rule, then another blank row. Pushed before each settled
// turn, between live-tail turns, and at the frozen↔live boundary.
inline maya::Element gap_row() {
    using namespace maya::dsl;
    return v(blank(),
             maya::Conversation::divider(),
             blank()).build();
}
inline constexpr int kGapRows = 3;

// Compaction-boundary divider: single-row `≡ Conversation compacted`
// rule, emitted before a run that begins on a compaction boundary.
inline maya::Element compaction_divider_row() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";   // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = muted;
    return maya::Turn{std::move(cfg)}.build();
}

// True iff a run starting at message index `idx` opens on a compaction
// boundary (and must therefore be preceded by the divider). Shared so
// the frozen and live builders agree on WHEN the divider appears, not
// just what it looks like.
inline bool compaction_boundary_at(
        const std::vector<Thread::CompactionRecord>& recs,
        std::size_t idx, std::size_t total) {
    for (const auto& rec : recs) {
        if (rec.up_to_index == idx && rec.up_to_index > 0
            && rec.up_to_index <= total) return true;
    }
    return false;
}

} // namespace agentty::ui
