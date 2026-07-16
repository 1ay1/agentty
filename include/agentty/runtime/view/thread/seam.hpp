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
#include <maya/render/cache_id.hpp>
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

// Compaction-boundary divider: a single-row centered labeled rule
//   ─────  ≡ Conversation compacted  ─────
// emitted before a run that begins on a compaction boundary. Rendered
// as a real horizontal separator (not a speaker Turn with a hanging
// left rail, which read as a broken empty message) so the boundary
// looks like the section break it is — the same visual family as
// maya::Conversation::divider(), just carrying a centered label.
//
// MUST stay exactly ONE row: freeze_range (frozen.cpp) and
// build_live_tail (conversation.cpp) both emit this at the boundary, and
// any height delta at the freeze seam ghosts committed scrollback rows
// (INLINE_SCROLLBACK.md pin #3). A single width-aware text() row keeps
// the seam height-stable by construction.
inline maya::Element compaction_divider_row() {
    using namespace maya::dsl;
    return maya::detail::component([](int w, int /*h*/) -> maya::Element {
        if (w <= 0) return blank().build();
        // "≡ Conversation compacted" centered, flanked by ─ rules. Built
        // as ONE string so the row is a single TextElement (exactly one
        // row, byte-identical across the freeze seam) — the same shape as
        // maya::Conversation::divider_rule(), just carrying a label.
        const std::string label = "\xe2\x89\xa1 Conversation compacted";
        // ≡ is a 3-byte glyph that renders in 1 column; the rest is ASCII.
        const int label_cells = static_cast<int>(label.size()) - 2;
        const int rule_total  = w - label_cells - 2;   // 1 space each side
        std::string line;
        if (rule_total < 2) {
            line = "   " + label;                       // too narrow to flank
        } else {
            const int left  = rule_total / 2;
            const int right = rule_total - left;
            for (int i = 0; i < left; ++i)  line += "\xe2\x94\x80";   // ─
            line += ' ';
            line += label;
            line += ' ';
            for (int i = 0; i < right; ++i) line += "\xe2\x94\x80";
        }
        return text(std::move(line),
                    maya::Style{}.with_fg(muted).with_dim()).build();
    })
    .hash_id(maya::CacheIdBuilder{}
        .add(std::string_view{"agentty.compaction.divider"})
        .build());
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
