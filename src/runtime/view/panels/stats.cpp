// stats.cpp — the stats panel. Table-driven: no per-tab branches.
//
// The whole view is:
//
//   for each Section in the active tab's TabDesc
//       extract() its Metrics
//       emit them onto one maya::StatSheet
//
// Adding a tab touches this file NOT AT ALL. That is the design's claim,
// and this file is where it either holds or does not.
//
// Formatting lives in domain/stats/unit.hpp (one format() for the whole
// panel) and alignment lives in maya::StatSheet (one measurement for the
// whole sheet), so this file owns exactly one thing: which theme colour a
// domain hue slot maps to.

#include "panels_prologue.hpp"

#include <maya/widget/stat_sheet.hpp>
#include <maya/widget/tab_strip.hpp>

#include "agentty/domain/stats/tabs.hpp"
#include "agentty/runtime/panel/stats.hpp"

namespace agentty::ui {
namespace {

using maya::StatEntry;
using maya::StatSheet;

// The ONE place a domain hue slot becomes a colour. domain/ deliberately
// does not know about the theme, so the mapping lives here — and lives
// once, rather than in each extractor.
[[nodiscard]] maya::Color hue_of(int slot) {
    switch (slot) {
        case 1:  return success;
        case 2:  return warn;
        case 3:  return danger;
        case 4:  return accent;
        default: return muted;
    }
}

// One section → sheet rows. The Viz says which fields the extractor
// filled, so this switch is over PRESENTATION, not over tabs — it does not
// grow when a tab is added.
void emit_section(const stats::Section& sec, const stats::Facts& f,
                  StatSheet& sheet, std::vector<stats::Metric>& scratch) {
    scratch.clear();
    sec.extract(f, scratch);
    if (scratch.empty()) return;   // a section with nothing to say draws nothing

    // A blank SEPARATES sections; it does not precede the first one.
    // Leading whitespace at the top of a panel reads as a rendering fault
    // rather than as breathing room.
    //
    // Applied whether or not the section has a HEADING: a headingless
    // band butted straight against the table above it reads as one more
    // row of that table, which is the opposite of what a band says.
    //
    // Bands and plots also take a COLUMN BREAK. They are full-width forms
    // — a band claims its segments are a whole, a plot needs horizontal
    // room to be a curve — and squeezed into a half-width column both
    // read as broken rather than as small. The break is inert when the
    // sheet is not splitting.
    const bool full_width = sec.viz == stats::Viz::Band
                         || sec.viz == stats::Viz::Plot;
    if (!sheet.empty()) {
        if (full_width) sheet.column_break();
        sheet.blank();
    }
    if (!sec.heading.empty()) sheet.heading(std::string{sec.heading});

    switch (sec.viz) {
        case stats::Viz::Hero:
            for (const auto& mt : scratch)
                sheet.hero(mt.label, mt.detail);
            break;

        case stats::Viz::Kv: {
            // A key/value row still gets a BAR, scaled against the largest
            // value in its own section. A column of bare numbers makes the
            // reader compare digit strings; a column of bars makes the
            // shape of the section visible without reading anything.
            //
            // Scaled per SECTION rather than per tab because a section is
            // the set the author already decided belongs together — and
            // mixing units on one scale (a token count against a duration)
            // would draw a comparison that means nothing.
            //
            // Only when the section is homogeneous and has something to
            // compare: one row has no shape, and a section spanning units
            // would be measuring milliseconds against tokens.
            bool same_unit = true;
            double peak = 0;
            for (const auto& mt : scratch) {
                if (mt.unit != scratch.front().unit) same_unit = false;
                if (mt.value > peak) peak = mt.value;
            }
            // A ratio is already a share of a known whole; bar-ing it
            // against the section's largest ratio would rescale it into a
            // number that is not the one printed beside it.
            const bool ratio = scratch.front().unit == stats::Unit::Ratio;
            const bool bar = same_unit && !ratio && scratch.size() > 1 && peak > 0;
            for (const auto& mt : scratch)
                sheet.entry({.label  = mt.label,
                             .value  = stats::format(mt.unit, mt.value),
                             .detail = mt.detail,
                             .share  = bar ? mt.value / peak : -1.0,
                             .hue    = bar ? std::optional<maya::Color>{muted}
                                           : std::nullopt});
            break;
        }

        case stats::Viz::Bars:
            for (const auto& mt : scratch)
                sheet.entry({.label  = mt.label,
                             .value  = stats::format(mt.unit, mt.value),
                             .detail = mt.detail,
                             .share  = mt.share()});
            break;

        case stats::Viz::Spark: {
            // A trend strip where the metric has one, a section-scaled bar
            // where it does not — so a Spark section can mix a rate that
            // has history with a plain count and still have every row
            // carry a visual. The blocks are filled by construction
            // (▁▂▃▄▅▆▇█ are solid from the baseline up), which is what makes
            // an eight-sample strip read as an area rather than as dots.
            double peak = 0;
            for (const auto& mt : scratch)
                if (mt.series.empty() && mt.value > peak) peak = mt.value;
            for (const auto& mt : scratch)
                sheet.entry({.label  = mt.label,
                             .value  = stats::format(mt.unit, mt.value),
                             .detail = mt.detail,
                             .share  = mt.series.empty() && peak > 0
                                         ? mt.value / peak : -1.0,
                             .spark  = mt.series,
                             .hue    = mt.series.empty()
                                         ? std::optional<maya::Color>{muted}
                                         : std::optional<maya::Color>{accent}});
            break;
        }

        case stats::Viz::Dist:
            // A distribution reads as a histogram lying on its side: the
            // bucket range is the label and the bar is how many landed
            // there, so the SHAPE is the thing you see. Scaled against the
            // tallest bucket (Metric::of) rather than the total, because
            // the question is "where did they cluster", and against a
            // total every bucket of a wide spread is a stub.
            for (const auto& mt : scratch)
                sheet.entry({.label  = mt.label,
                             .value  = mt.value > 0
                                         ? stats::format(mt.unit, mt.value)
                                         : "",
                             .detail = mt.detail,
                             .share  = mt.share(),
                             .hue    = accent});
            break;

        case stats::Viz::Band:
            for (const auto& mt : scratch) {
                maya::StatBand band;
                band.caption = mt.label;
                for (const auto& p : mt.parts)
                    band.segments.push_back({p.label, p.value, hue_of(p.hue)});
                sheet.band(std::move(band));
            }
            break;

        case stats::Viz::Plot:
            for (const auto& mt : scratch) {
                if (mt.series.size() < 2) continue;
                double hi = 0;
                for (double v : mt.series) if (v > hi) hi = v;
                sheet.plot({.caption    = mt.label,
                            .series     = mt.series,
                            // 4 rows, not 5. The panel viewport is 14 rows
                            // and a tab's tables already claim most of it;
                            // a figure that pushes itself past the fold is
                            // a figure nobody scrolls to. At 4 braille
                            // rows the plot still carries 16 dot rows of
                            // vertical resolution.
                            .rows       = 4,
                            .hue        = accent,
                            .peak_label = stats::format(mt.unit, hi),
                            .base_label = "0"});
            }
            break;
    }
}

}  // namespace

Element stats_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Stats>();
    if (!o) return nothing();

    // One incremental refresh. On a settled thread this folds nothing; while
    // streaming it folds exactly the live tail. See domain/stats/facts.hpp
    // for why the cursor and the epoch guard are shaped the way they are.
    const stats::Facts& f = o->projection.refresh(m.d.current);

    // A tab can become unavailable under the user (the last tool call was
    // rewound away). Landing on it would show an empty panel with no way to
    // understand why, so the selection is corrected here rather than
    // defended in every extractor.
    const auto visible = stats::visible_tabs(f);
    stats::Tab active = o->tab;
    if (!stats::tab_available(active, f)) active = visible.front();
    o->tab = active;

    maya::panel::Config cfg;
    cfg.title    = "Stats";
    cfg.subtitle = std::string{stats::tab_subtitle(active)};
    cfg.accent   = accent;

    // Tabs are the WIDGET's chrome, not this host's: it owns the padding,
    // the selected treatment and how the strip degrades on a narrow frame,
    // so every tabbed panel looks the same by construction.
    //
    // Editor marking because these tabs are PEERS you switch between, not
    // views of one underlying thing — " │ " dividers carry the structure
    // and there is no underline rule. Filled because a mark that works by
    // CONTRAST says nothing about the first tab in the strip; a chip
    // states "this is the live view" on its own.
    //
    // The strip shows only the AVAILABLE tabs, so a session that never ran
    // a tool has no Tools chip to land on — an empty tab is a tab admitting
    // it should not have been drawn.
    cfg.tabs.reserve(visible.size());
    for (auto t : visible) cfg.tabs.emplace_back(stats::tab_title(t));
    cfg.tab_active = 0;
    for (std::size_t i = 0; i < visible.size(); ++i)
        if (visible[i] == active) { cfg.tab_active = static_cast<int>(i); break; }
    cfg.tab_mark = maya::TabMark::Editor;
    cfg.tab_fill = true;

    // The panel body: one sheet, every section on it. Because it is ONE
    // sheet rather than one per section, every row on the tab is measured
    // against the same columns — which is the difference between a readout
    // and a stack of unrelated tables.
    StatSheet sheet;
    sheet.indent(1);
    // The panel wraps the sheet in its own chrome and hands it the OUTER
    // width: a border column plus 3 columns of inner pad on the left, and
    // 2 pad + 1 border on the right, with the scrollbar riding inside that
    // right pad. The sheet can see none of it, so its full-width forms
    // (bands, plots) run past the right border — measured at three widths,
    // a band ran the full 76 columns of a 76-column panel, and a plot's
    // peak label lost its last character to the clip.
    //
    // 7 = the 4 columns of left chrome the sheet's own indent(1) does not
    // cover, plus the 3 on the right. Reserved unconditionally rather than
    // only when scrolling: a band whose width changes as content grows past
    // the viewport is a layout that shifts under the reader for no reason
    // they can see.
    sheet.reserve_right(7);
    // Flow into columns once the surface can afford them. 34 columns is the
    // narrowest a stat row stays readable at — label, a shrunken track and
    // a right-aligned value — so a 76-column panel stays single-column and
    // a 110+ one splits. Capped at 2: a third column on a very wide
    // terminal makes the eye travel further than scrolling would have.
    //
    // The alternative was a fixed breakpoint. A minimum WIDTH is the
    // honest spelling: it says what a column needs rather than guessing
    // which terminal sizes exist.
    sheet.columns(34, 2);
    sheet.theme.label   = fg;
    sheet.theme.value   = fg;
    sheet.theme.detail  = muted;
    sheet.theme.heading = accent;
    sheet.theme.bar     = accent;
    sheet.theme.track   = muted;
    sheet.theme.hero    = accent;

    std::vector<stats::Metric> scratch;
    scratch.reserve(32);
    for (const auto& sec : stats::tab_desc(active).sections)
        emit_section(sec, f, sheet, scratch);

    cfg.prebuilt.push_back(sheet.build());

    // Read-only: no cursor. A selection highlight on rows nothing can be
    // done to is a promise the panel cannot keep.
    cfg.selected   = -1;
    cfg.scroll     = &m.ui.stats_scroll;
    // Body height. `viewport_h` is the panel's, and the plot rows have to
    // fit inside it alongside everything else — a five-row figure at the
    // bottom of a full tab is a figure the user only ever sees the top of.
    cfg.viewport_h = panel_detail::panel_viewport_h();

    cfg.note = visible.size() > 1 ? "tab  switch view   \xe2\x86\x91\xe2\x86\x93  scroll   esc  close"
                                  : "\xe2\x86\x91\xe2\x86\x93  scroll   esc  close";
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
