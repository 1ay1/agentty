#pragma once
// agentty::stats — Metric and Section: the row vocabulary.
//
// A Metric is a number plus the unit that says how to spell it plus the
// denominator it is a share OF. Three fields, because those are the three
// things every statistic in the panel turns out to need and no statistic
// needs a fourth.
//
// `of` is CARRIED rather than recomputed at the view. Two consumers
// dividing independently disagree in the last digit, and a panel whose
// shares do not sum to one is a panel nobody trusts twice.

#include <cstdint>
#include <string>
#include <vector>

#include "agentty/domain/stats/unit.hpp"

namespace agentty::stats {

struct Facts;   // fold input; extractors read it

struct Metric {
    // One string field, not string_view + owner. Real labels are model ids
    // and tool names — under 23 chars, so SSO means the "allocation"
    // argument for splitting them was never real, and two fields
    // describing one name is the shape this codebase keeps deleting.
    std::string label;
    Unit        unit  = Unit::Count;
    double      value = 0;
    double      of    = 0;      // 0 = no share, no bar
    // Trailing dim note. Its own column in the sheet, so notes align
    // instead of trailing raggedly off values of different widths.
    std::string detail;
    // Segments for a Band section: the parts of a whole. Empty otherwise.
    struct Part { std::string label; double value = 0; int hue = 0; };
    std::vector<Part> parts;
    // Samples for a Spark / Plot section.
    std::vector<double> series;

    [[nodiscard]] double share() const noexcept {
        return of > 0 ? value / of : -1.0;
    }
};

// How a section is drawn. This names which FIELDS the extractor fills, not
// which widget gets called — every one of them lands in the same
// maya::StatSheet, which is what keeps the rows aligned with each other.
enum class Viz : std::uint8_t {
    Kv,     // label + value               (no bar)
    Bars,   // label + value + share bar    (ranked)
    Spark,  // label + value + trend strip  (inline, one row)
    Band,   // one full-width composition bar + legend
    // The same composition as a RING. Band and donut fail in opposite
    // directions and that is why both exist: a band is exact and compact
    // but a 1-column sliver is easy to miss, while a ring makes the eye
    // compare ANGLES, where a thin wedge against a circle is obvious. Use
    // the ring when the composition IS the tab's answer, the band when it
    // is one fact among several.
    Donut,
    Plot,   // a multi-row braille/block figure
    Hero,   // the headline figure and the sentence it answers
    // A DISTRIBUTION: one row per occupied latency bucket, so the SHAPE is
    // visible rather than two quantiles of it. 95 fast calls and 5 slow
    // ones has the same mean as 100 medium ones and means something
    // completely different — which is invisible in "p50 / p95" and obvious
    // the moment the buckets are drawn.
    Dist,
};

struct Section {
    std::string_view heading;   // empty = no heading row
    Viz              viz;
    // Writes into a caller-owned scratch vector, so a steady-state frame
    // allocates nothing.
    void (*extract)(const Facts&, std::vector<Metric>& out);
};

}  // namespace agentty::stats
