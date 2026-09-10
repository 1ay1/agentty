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

#include <cmath>

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

// A CATEGORICAL ramp, for charts whose slices are named things rather
// than states. hue_of() is a SEMANTIC map — green means good, red means
// failed — and a pie of seven tool names has no semantics to encode: it
// needs seven distinguishable colours, and reusing the status palette
// would imply `grep` is somehow the failure case.
//
// Ordered so ADJACENT slices contrast. A ramp that walks the spectrum
// puts two blues next to each other, and neighbouring wedges are exactly
// the pair a reader has to tell apart.
[[nodiscard]] maya::Color series_hue(std::size_t i) {
    static const maya::Color kRamp[] = {
        accent,                       // magenta
        info,                         // blue
        success,                      // green
        warn,                         // yellow
        highlight,                    // cyan
        maya::Color::bright_magenta(),
        maya::Color::bright_blue(),
        maya::Color::bright_green(),
    };
    constexpr std::size_t n = sizeof(kRamp) / sizeof(kRamp[0]);
    return kRamp[i % n];
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
                         || sec.viz == stats::Viz::Donut
                         || sec.viz == stats::Viz::Hist
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

        case stats::Viz::Donut:
            for (const auto& mt : scratch) {
                maya::StatDonut ring;
                ring.caption = mt.label;
                // The centre carries the headline. A donut with an empty
                // hole wastes the one place the reader is already looking
                // — and the figure the ring is decorating belongs there,
                // not in a row underneath it.
                ring.center     = mt.detail;
                ring.center_sub = "";
                // Two palettes, and the METRIC says which. A state
                // (cache hit / miss, waiting / generating) carries
                // semantics worth encoding; a set of names (tools,
                // models) carries none, and reusing the status palette
                // for names would imply one of the tools is the failure
                // case. Read off the flag rather than inferred from the
                // values, so a categorical set whose first slot happens
                // to be non-zero cannot silently pick the wrong table.
                std::size_t i = 0;
                for (const auto& p : mt.parts) {
                    ring.segments.push_back(
                        {p.label, p.value,
                         mt.categorical ? series_hue(i) : hue_of(p.hue)});
                    ++i;
                }
                sheet.donut(std::move(ring));
            }
            break;

        case stats::Viz::Hist: {
            // The Dist extractors already produce one Metric per bucket
            // with the range as its label — the vertical form is the same
            // data turned ninety degrees, so it reuses them rather than
            // needing a second extractor per histogram.
            maya::StatHistogram hist;
            double peak = 0;
            for (const auto& mt : scratch) {
                // The label is a RANGE ("64ms–128ms"); the axis wants a
                // tick, so take the lower bound. A full range under every
                // third column is unreadable at any width.
                std::string tick = mt.label;
                if (const auto dash = tick.find("\xe2\x80\x93");
                    dash != std::string::npos)
                    tick = tick.substr(0, dash);
                hist.buckets.push_back({std::move(tick), mt.value});
                if (mt.value > peak) peak = mt.value;
            }
            if (hist.buckets.empty()) break;
            hist.rows       = 5;
            hist.hue        = accent;
            hist.col_width  = 4;
            // A tick per row, top-down: the value a bar reaching that row
            // represents. One peak label at the top gives you the ceiling
            // and nothing else, so reading any other bar means estimating
            // its fraction of a number at the far end of the figure.
            //
            // Rounded to whole counts and de-duplicated: on a histogram
            // whose peak is 3, five rows would otherwise print "3 2 2 1 1"
            // and the repeats read as a rendering fault rather than as
            // rounding. A blank row is an honest "no new tick here".
            //
            // A tick labels its row's TOP edge, so the bottom row's edge
            // can round to zero — and printing "0" there claims the row
            // IS the zero line when the baseline rule below it is. Left
            // blank instead: the axis already says where zero is.
            hist.y_labels.reserve(static_cast<std::size_t>(hist.rows));
            std::string prev;
            for (int r = 0; r < hist.rows; ++r) {
                const double at = peak * static_cast<double>(hist.rows - r)
                                       / static_cast<double>(hist.rows);
                const double whole = std::floor(at + 0.5);
                std::string lb = whole >= 1.0
                    ? stats::format(stats::Unit::Count, whole)
                    : std::string{};
                if (lb == prev) lb.clear();
                else if (!lb.empty()) prev = lb;
                hist.y_labels.push_back(std::move(lb));
            }
            sheet.histogram(std::move(hist));
            break;
        }

        case stats::Viz::Band:
            for (const auto& mt : scratch) {
                maya::StatBand band;
                band.caption = mt.label;
                std::size_t bi = 0;
                for (const auto& p : mt.parts) {
                    band.segments.push_back(
                        {p.label, p.value,
                         mt.categorical ? series_hue(bi) : hue_of(p.hue)});
                    ++bi;
                }
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
                            // 6 rows. At 4 the filled area was a squat
                            // band where every column looked the same
                            // height — a chart needs vertical range for
                            // the SHAPE to be the thing you see, and the
                            // panel scrolls, so a figure worth reading is
                            // worth two more rows.
                            .rows       = 6,
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
    // Flow into columns once the surface can afford them — "afford"
    // being the sheet's judgement, not this panel's. It used to be a
    // number here (46 columns, eyeballed as label + track + value), and a
    // caller-side guess at a widget's internal geometry is a guess that
    // goes stale: it omitted the gaps and the detail note, so real need
    // was ~62, and every width in 120..128 bought a second column the
    // sheet then had to draw with no chart in it. Widening the terminal
    // DELETED the bars.
    //
    // Now the sheet searches the column count itself and only takes a
    // split whose every slice still fits with its track and labels
    // intact, so the count is monotonic in width by construction. What is
    // left here is the one thing this panel legitimately knows: the CAP.
    // Two, because a third column on a very wide terminal makes the eye
    // travel further than scrolling would have.
    sheet.columns(2);
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
    // Body height. panel_viewport_h() is clamped to kViewportH (14) — the
    // right ceiling for a PICKER, whose rows are interchangeable and where
    // 14 of them is plenty to choose from. This panel is a document: its
    // sections are not alternatives, and capping it at 14 dropped whole
    // figures below the fold on an 80-row terminal that had room for all
    // of them.
    //
    // So it takes what the terminal actually offers, with the same chrome
    // reserve the shared helper uses. Content past that still scrolls.
    cfg.viewport_h = std::max(panel_detail::panel_viewport_h(),
                              panel_detail::panel_terminal_rows()
                                  - panel_detail::kPickerChromeRows - 1);

    cfg.note = visible.size() > 1 ? "tab  switch view   \xe2\x86\x91\xe2\x86\x93  scroll   esc  close"
                                  : "\xe2\x86\x91\xe2\x86\x93  scroll   esc  close";
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
