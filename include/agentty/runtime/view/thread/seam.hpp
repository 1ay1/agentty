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
//   ┈┈┈┈┈  ≡ Conversation compacted  ┈┈┈┈┈
// emitted before a run that begins on a compaction boundary. Rendered
// as a real horizontal separator (not a speaker Turn with a hanging
// left rail, which read as a broken empty message) so the boundary
// looks like the section break it is. Visually distinct from the
// inter-turn `─` rule: a lighter QUAD-DASH rule (┈) flanks the label,
// the `≡` glyph carries the brand accent, and the label itself sits a
// tone brighter than the rule so the event reads at a glance instead
// of dissolving into chrome.
//
// MUST stay exactly ONE row: freeze_range (frozen.cpp) and
// build_live_tail (conversation.cpp) both emit this at the boundary, and
// any height delta at the freeze seam ghosts committed scrollback rows
// (INLINE_SCROLLBACK.md pin #3). A single-row h() of styled segments
// keeps the seam height-stable by construction — and both builders call
// THIS function, so the bytes cannot drift either.
//
// The label literal "Conversation compacted" is load-bearing: the seam
// tests (midrun_seam_test) grep for it to count divider copies.
inline maya::Element compaction_divider_row() {
    using namespace maya::dsl;
    return maya::detail::component([](int w, int /*h*/) -> maya::Element {
        if (w <= 0) return blank().build();
        const std::string glyph = "\xe2\x89\xa1";              // ≡ (1 col)
        const std::string label = "Conversation compacted";    // ASCII
        // cells: glyph(1) + space(1) + label + two spaces each side
        const int label_cells = 2 + static_cast<int>(label.size());
        const int rule_total  = w - label_cells - 4;
        if (rule_total < 4) {
            // Too narrow to flank — left-aligned compact form.
            return h(
                text("   "),
                text(glyph + " ", maya::Style{}.with_fg(accent)),
                text(label, maya::Style{}.with_fg(text_secondary).with_dim())
            ).build();
        }
        const int left  = rule_total / 2;
        const int right = rule_total - left;
        std::string lrule, rrule;
        lrule.reserve(static_cast<std::size_t>(left) * 3);
        rrule.reserve(static_cast<std::size_t>(right) * 3);
        for (int i = 0; i < left; ++i)  lrule += "\xe2\x94\x88";   // ┈
        for (int i = 0; i < right; ++i) rrule += "\xe2\x94\x88";
        return h(
            text(std::move(lrule),
                 maya::Style{}.with_fg(muted).with_dim()),
            text("  "),
            text(glyph, maya::Style{}.with_fg(accent)),
            text(" "),
            text(label, maya::Style{}.with_fg(text_secondary).with_dim()),
            text("  "),
            text(std::move(rrule),
                 maya::Style{}.with_fg(muted).with_dim())
        ).build();
    })
    .hash_id(maya::CacheIdBuilder{}
        .add(std::string_view{"agentty.compaction.divider.v2"})
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
