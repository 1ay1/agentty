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

namespace {

// ── The Item path ────────────────────────────────────────────────────────
//
// A stats ROW is a panel Item: label · drawn control · note. That
// vocabulary already exists, and with it the frame, the viewport, the
// scrollbar and the row geometry — all of which StatSheet re-implements
// and then hands Panel one opaque prebuilt blob so none of it applies.
// Every sizing bug in this subsystem came from that duplication.
//
// What stays with StatSheet is the thing Panel genuinely does not do:
// flowing SECTIONS side by side when the surface is wide. That is a
// stats-only concern, and pushing it into a widget fourteen panels share
// would be the same mistake in the other direction.
//
// Returns false when the section is not a row kind, so the caller falls
// through to the sheet and the migration proceeds one Viz at a time.
bool emit_items(const stats::Section& sec, const stats::Facts& f,
                std::vector<maya::panel::Item>& out,
                std::vector<stats::Metric>& scratch,
                maya::Color accent, maya::Color muted) {
    using maya::panel::Item;
    using maya::panel::Meter;
    using maya::panel::Spark;

    // Every Viz now has a panel Control. StatSheet's remaining job is
    // flowing SECTIONS into columns -- a stats-only concern -- and even
    // that goes once the panel can do it.
    (void)0;

    scratch.clear();
    sec.extract(f, scratch);
    if (scratch.empty()) return true;   // handled: an empty section draws nothing

    if (!out.empty()) out.push_back(Item{});          // blank separator

    // A hero is a headline, not a table row: a big number and the sentence
    // that qualifies it, reading as one phrase.
    //
    // Both in `leading`, not number-in-leading + caption-in-origin. The
    // origin cell is RIGHT-aligned — correct for a provenance tag on a
    // settings row, wrong here, where it flung "of 8 routed turns ran
    // below the Strategic model" to the far edge and left the number
    // stranded alone on the left. A headline is one phrase; it is laid out
    // as one string.
    // A band is a composition: one full-width bar plus its key. It owns
    // its row, so it needs no label lane -- the section heading above it is
    // its title.
    if (sec.viz == stats::Viz::Band) {
        for (const auto& mt : scratch) {
            maya::panel::Band band;
            band.caption = mt.label;
            std::size_t bi = 0;
            for (const auto& p : mt.parts) {
                band.segments.push_back(
                    {p.label, p.value,
                     mt.categorical ? series_hue(bi) : hue_of(p.hue)});
                ++bi;
            }
            Item it;
            it.control = std::move(band);
            out.push_back(std::move(it));
        }
        return true;
    }

    // A ring: the composition where the composition IS the answer.
    if (sec.viz == stats::Viz::Donut) {
        for (const auto& mt : scratch) {
            maya::panel::Donut ring;
            ring.caption = mt.label;
            ring.center  = mt.detail;
            std::size_t i = 0;
            for (const auto& p : mt.parts) {
                ring.segments.push_back(
                    {p.label, p.value,
                     mt.categorical ? series_hue(i) : hue_of(p.hue)});
                ++i;
            }
            Item it;
            it.control = std::move(ring);
            out.push_back(std::move(it));
        }
        return true;
    }

    // A curve: a series over turns, where direction is the question.
    if (sec.viz == stats::Viz::Plot) {
        for (const auto& mt : scratch) {
            if (mt.series.empty()) continue;
            double hi = 0;
            for (double v : mt.series) hi = std::max(hi, v);
            maya::panel::Plot plot;
            plot.series     = mt.series;
            plot.rows       = 6;
            plot.caption    = mt.label;
            plot.peak_label = stats::format(mt.unit, hi);
            plot.base_label = "0";
            plot.hue        = accent;
            Item it;
            it.control = std::move(plot);
            out.push_back(std::move(it));
        }
        return true;
    }

    // A distribution, drawn as columns. The Dist extractors already
    // produce one Metric per bucket with the range as its label, so the
    // vertical form reuses them rather than needing a second extractor.
    if (sec.viz == stats::Viz::Hist || sec.viz == stats::Viz::Dist) {
        maya::panel::Hist hist;
        double peak = 0;
        for (const auto& mt : scratch) {
            // The label is a RANGE ("64ms-128ms"); the axis wants a tick,
            // so take the lower bound -- a full range under every third
            // column is unreadable at any width.
            std::string tick = mt.label;
            if (const auto dash = tick.find("\xe2\x80\x93");
                dash != std::string::npos)
                tick = tick.substr(0, dash);
            hist.buckets.push_back({std::move(tick), mt.value});
            peak = std::max(peak, mt.value);
        }
        if (hist.buckets.empty()) return true;
        hist.rows      = 5;
        hist.col_width = 4;
        hist.caption   = std::string{sec.heading};
        hist.hue       = accent;
        // A tick per row, top-down, rounded to whole counts and
        // de-duplicated: five rows over a peak of 3 would otherwise print
        // "3 2 2 1 1" and the repeats read as a rendering fault.
        std::string prev;
        for (int r = 0; r < hist.rows; ++r) {
            const double at = peak * static_cast<double>(hist.rows - r)
                                   / static_cast<double>(hist.rows);
            const double whole = std::floor(at + 0.5);
            std::string lb = whole >= 1.0
                ? stats::format(stats::Unit::Count, whole) : std::string{};
            if (!lb.empty() && lb == prev) lb.clear(); else if (!lb.empty()) prev = lb;
            hist.y_labels.push_back(std::move(lb));
        }
        Item it;
        it.control = std::move(hist);
        out.push_back(std::move(it));
        return true;
    }

    if (sec.viz == stats::Viz::Hero) {
        for (const auto& mt : scratch) {
            Item it;
            it.leading = mt.detail.empty()
                       ? mt.label
                       : mt.label + "  " + mt.detail;
            // The NUMBER carries the accent; the qualifier is prose. The
            // panel paints `leading` in one style, so the emphasis that
            // matters is the row's, not the number's alone.
            it.leading_style = maya::Style{}.with_fg(accent).with_bold();
            out.push_back(std::move(it));
        }
        return true;
    }
    if (!sec.heading.empty()) {
        // Header is a MARKER kind: the text rides in `leading` and the
        // control says "render this as a section header".
        Item h;
        h.leading = std::string{sec.heading};
        h.control = maya::panel::Header{};
        out.push_back(std::move(h));
    }

    // The share each row's bar encodes — the ONE place the three row kinds
    // differ, decided once here rather than in three branches that drift.
    //
    // Kv scales against its section's largest value: a column of bare
    // numbers makes the reader compare digit strings, a column of bars
    // makes the section's shape visible without reading anything. Only
    // when the section is homogeneous and has something to compare — one
    // row has no shape, mixed units would measure milliseconds against
    // tokens, and a ratio is already a share of a known whole.
    double peak = 0;
    bool   scaled = false;
    if (sec.viz == stats::Viz::Kv) {
        bool same_unit = true;
        for (const auto& mt : scratch) {
            if (mt.unit != scratch.front().unit) same_unit = false;
            if (mt.value > peak) peak = mt.value;
        }
        scaled = same_unit && scratch.front().unit != stats::Unit::Ratio
              && scratch.size() > 1 && peak > 0;
    } else if (sec.viz == stats::Viz::Spark) {
        for (const auto& mt : scratch)
            if (mt.series.empty() && mt.value > peak) peak = mt.value;
        scaled = peak > 0;
    }

    for (const auto& mt : scratch) {
        Item it;
        it.leading = mt.label;
        it.origin  = mt.detail;
        const std::string value = stats::format(mt.unit, mt.value);

        // The value travels WITH the control, not in `trailing`: a control
        // replaces that cell rather than sitting beside it, so a row that
        // set both silently lost its number.
        if (!mt.series.empty()) {
            it.control = Spark{.series = mt.series, .value = value,
                               .hue = accent};
        } else {
            const double share =
                sec.viz == stats::Viz::Bars ? mt.share()
              : (scaled ? mt.value / peak : -1.0);
            if (share >= 0.0)
                it.control = Meter{.share = share, .value = value,
                                   .hue = muted};
            else
                it.trailing = value;   // no picture: the plain value cell
        }
        out.push_back(std::move(it));
    }
    return true;
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
    // The chrome the sheet cannot see. Panel accounts for its own frame
    // when it lays out `items`; a `prebuilt` element is pushed into the
    // body as-is and measured against the body width, so it never learns
    // about the border, the padding or the scrollbar gutter. 7 = the 4
    // columns of left chrome indent(1) does not cover, plus the 3 on the
    // right. Deleted by the port, not by an argument.
    sheet.reserve_right(7);
    // No reserve. maya::Panel subtracts its own chrome -- border, padding
    // and the scrollbar gutter -- before the body is laid out, so the width
    // the sheet is handed is a width it may actually paint in.
    //
    // This used to be `sheet.reserve_right(7)`: a hand-counted constant
    // describing the CONTAINER's internals, living in the CONTENT. It was
    // derived by measuring at three widths, and it was circular by
    // construction -- the scrollbar appeared only when content exceeded the
    // viewport, so the correct reserve was 6 or 7 depending on a condition
    // that itself depended on the width the reserve decided. That is why it
    // was right at 76 and wrong at 68.
    // Flow into columns once the surface can afford them. 46 columns is
    // what a stat row needs to stay READABLE — label, a full-width track
    // and a right-aligned value with its note — not the narrowest it can
    // be squeezed to. At 34 an 80-column terminal split into two columns
    // that each truncated their labels, which is worse than scrolling.
    // Capped at 2: a third column on a very wide terminal makes the eye
    // travel further than scrolling would have.
    //
    // The alternative was a fixed breakpoint. A minimum WIDTH is the
    // honest spelling: it says what a column needs rather than guessing
    // which terminal sizes exist.
    sheet.columns(46, 2);
    sheet.theme.label   = fg;
    sheet.theme.value   = fg;
    sheet.theme.detail  = muted;
    sheet.theme.heading = accent;
    sheet.theme.bar     = accent;
    sheet.theme.track   = muted;
    sheet.theme.hero    = accent;

    std::vector<stats::Metric> scratch;
    scratch.reserve(32);

    // A tab whose sections are ALL row kinds goes through the panel's own
    // Item vocabulary; anything carrying a figure still builds a sheet.
    //
    // Per-tab rather than per-section because items and prebuilt are
    // ALTERNATIVES in Config, not a sequence. Splitting on the tab keeps
    // each one whole and lets the migration proceed a tab at a time, each
    // diffed against the golden harness.
    const auto& sections = stats::tab_desc(active).sections;
    // Every Viz has a panel Control now, so every tab takes the Item path.
    constexpr bool all_rows = true;

    if (all_rows) {
        for (const auto& sec : sections)
            emit_items(sec, f, cfg.items, scratch, accent, muted);
    } else {
        for (const auto& sec : sections)
            emit_section(sec, f, sheet, scratch);
        cfg.prebuilt.push_back(sheet.build());
    }

    // Read-only: no cursor. A selection highlight on rows nothing can be
    // done to is a promise the panel cannot keep.
    cfg.selected   = -1;

    // Flow into columns once one would be wider than this.
    //
    // A stats tab is a DOCUMENT — sections you read, not alternatives you
    // pick between — which is exactly the shape column flow is for, and is
    // why the cursor above being absent is a precondition rather than a
    // coincidence. On a 200-column terminal a single column of rows is a
    // narrow ribbon with two thirds of the screen blank and figures below
    // the fold that would have fitted on it.
    //
    // 64 because that is about where a label─→value row stops being easy to
    // track across: past it the eye loses the line on the way to the
    // number. It is a READING measure, not a fitting one — which is the
    // point of stating a ceiling rather than a minimum.
    //
    // Everything else is maya's: how many columns that implies, dividing
    // the body exactly so no strip is left over, balancing the columns by
    // height, and keeping each figure sized to ITS COLUMN rather than to
    // the screen. This host supplies one number and no geometry — the
    // arithmetic it used to do here is what clipped "6.0s" to "6".
    cfg.col_max_width = 64;

    // ...but never split below what a stats row needs. A row is a label, a
    // meter and a value; at 36 columns the labels truncate ("Waiti…",
    // "Genera…") and the split has traded a too-wide line for a lossy one.
    // 52 is where the longest label in the fixture still lands whole.
    //
    // This is why a ceiling alone is not enough: 64 on a 76-column terminal
    // would otherwise split into two columns of 36, which is worse than the
    // single wide column it was trying to improve on.
    cfg.col_min_width = 52;

    cfg.scroll     = &o->scroll;
    // Body height: the same one every other panel uses.
    //
    // This was special-cased twice and both attempts were worse than the
    // shared helper. Taking the terminal's full height made short tabs sit
    // in a mostly-empty frame; pinning it to the tallest tab kept the
    // frame stable but sized it unlike every other panel in the app, so
    // opening stats felt like opening something else.
    //
    // A panel that matches its siblings is worth more than one tuned to
    // its own content. Content past the viewport scrolls, which is what
    // the scrollbar is for and what every other panel does with overflow.
    cfg.viewport_h = panel_detail::panel_viewport_h();

    // Hold that height rather than shrink-wrapping to the tab.
    //
    // The ONE thing stats needs that a picker does not. A picker shows one
    // list and shrink-wrapping it is right; these tabs differ in length, so
    // without this the frame resized every time the reader pressed tab —
    // 13 rows on one, 30 on another, the box jumping under the cursor.
    //
    // Same viewport as every other panel, just not renegotiated per tab.
    cfg.fixed_viewport = true;

    cfg.note = visible.size() > 1 ? "tab  switch view   \xe2\x86\x91\xe2\x86\x93  scroll   esc  close"
                                  : "\xe2\x86\x91\xe2\x86\x93  scroll   esc  close";
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
