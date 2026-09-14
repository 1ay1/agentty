// stats.cpp — the stats panel. Table-driven: no per-tab branches.
//
// The whole view is:
//
//   for each Section in the active tab's TabDesc
//       extract() its Metrics
//       build ONE card Element from them, chosen by the Section's Viz
//       push the card into cfg.prebuilt
//
// Adding a tab touches this file NOT AT ALL. That is the design's claim,
// and this file is where it either holds or does not.
//
// Formatting lives in domain/stats/unit.hpp (one format() for the whole
// panel), and each Viz maps to one standalone maya widget (Donut,
// Sparkline, Gauge, BarChart, Histogram, LineChart). The cards flow into
// responsive columns via maya::Panel's col_max_width layout, so this file
// owns exactly one thing: which theme colour a domain hue slot maps to.

#include "panels_prologue.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <maya/widget/bar_chart.hpp>
#include <maya/element/grid.hpp>
#include <maya/widget/donut.hpp>
#include <maya/widget/gauge.hpp>
#include <maya/widget/histogram.hpp>
#include <maya/widget/line_chart.hpp>
#include <maya/widget/sparkline.hpp>
#include <maya/widget/tab_strip.hpp>

#include "agentty/domain/stats/tabs.hpp"
#include "agentty/runtime/panel/stats.hpp"

namespace agentty::ui {
namespace {

// The card accent ramp — the agent_stats palette, verbatim. These are the
// soft high-value hues the example's dashboard is built on; the agentty
// theme's status colours (green=good, red=failed) are a SEMANTIC map and
// carry the wrong meaning for a grid of named categories.
namespace hue {
const maya::Color cyan   = maya::Color::rgb(120, 220, 232);
const maya::Color green  = maya::Color::rgb(150, 230, 160);
const maya::Color amber  = maya::Color::rgb(245, 200, 110);
const maya::Color red    = maya::Color::rgb(255, 130, 130);
const maya::Color violet = maya::Color::rgb(190, 160, 255);
const maya::Color blue   = maya::Color::rgb(120, 180, 255);
const maya::Color pink   = maya::Color::rgb(240, 150, 200);
const maya::Color teal   = maya::Color::rgb(120, 220, 200);
const maya::Color dim    = maya::Color::rgb(140, 150, 170);
}  // namespace hue

// A CATEGORICAL ramp, for charts whose slices are named things rather
// than states — the example's palette in its cycling order, so adjacent
// slices contrast instead of walking the spectrum into two neighbouring
// blues.
[[nodiscard]] maya::Color series_hue(int slot, std::size_t i) {
    // The catch-all slice draws muted. "other" is the ABSENCE of a
    // category, so giving it a category's colour makes it read as one
    // more of them — which it did: it collided with the largest slice.
    if (slot < 0) return hue::dim;
    static const maya::Color kRamp[] = {
        hue::cyan, hue::blue, hue::amber, hue::green,
        hue::violet, hue::teal, hue::red, hue::pink,
    };
    constexpr std::size_t n = sizeof(kRamp) / sizeof(kRamp[0]);
    return kRamp[i % n];
}

// ── card() — a bare titled section, matching the agent_stats look ─────────
//
// No border: the accent-coloured heading over the content IS the section
// boundary. A box around every card turns a page of figures into a grid of
// forms, and in two columns the boxes double up along the gutter. The
// vstack + padding(0,1) gives the breathing room a border would have,
// without the ink.
//
// Each card carries its OWN accent, cycling the ramp, so a grid of them
// reads as distinct panels rather than one wall of the same colour — the
// thing that makes the example's dashboard legible at a glance.
[[nodiscard]] Element card(std::string_view heading, maya::Color accent,
                           std::vector<Element> body) {
    std::vector<Element> rows;
    rows.reserve(body.size() + 2);
    if (!heading.empty()) {
        rows.push_back(text(std::string{heading}, fg_bold(accent)));
        rows.push_back(blank());
    }
    for (auto& e : body) rows.push_back(std::move(e));
    return (dsl::v(std::move(rows)) | padding(0, 1)).build();
}

// A section with nothing to say still draws its heading, so a tab does not
// silently drop a section the reader expected — the dim dash says "measured,
// empty" rather than "forgot to render".
[[nodiscard]] Element empty_placeholder() {
    return text("\xe2\x80\x94", fg_dim(hue::dim));   // — em dash
}

// ── Viz builders ──────────────────────────────────────────────────────────
//
// Each takes the extracted metrics and returns the card body (the elements
// under the heading). One function per PRESENTATION, so this file grows a
// case when a Viz is added, never when a tab is.

// Bars — one horizontal bar per metric, coloured by a hue cycle. Scaled to
// the largest value, or to `of` when the metrics carry a shared denominator
// (so shares of a whole read against that whole, not against each other).
[[nodiscard]] std::vector<Element> build_bars(const std::vector<stats::Metric>& ms) {
    std::vector<maya::Bar> bars;
    bars.reserve(ms.size());
    float peak = 0.0f;
    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];
        const float v = static_cast<float>(mt.value);
        peak = std::max(peak, std::max(v, static_cast<float>(mt.of)));
        bars.push_back({.label = mt.label,
                        .value = v,
                        .color = series_hue(0, i)});
    }
    std::vector<Element> out;
    out.push_back(maya::BarChart{std::move(bars), peak}.build());
    return out;
}

// Spark — an inline block sparkline per metric from its series, on ONE
// line: label, trace, then the metric's formatted value. show_last is off
// because it prints the raw last SAMPLE, which is a different number from
// the metric's formatted value — showing both invited the reader to
// reconcile "280" with "702ms".
// Spark — the trends of a section. A metric with NO series is not a trend:
// it is a plain figure that happens to share the section ("Turns using
// cache" beside "Hit rate"). Those are handed back through `figures` so
// the caller can put them in the READOUT column, where there is room for
// them — squeezing them under the chart cost the chart height and left the
// readout column half empty.
[[nodiscard]] std::vector<Element> build_sparks(const std::vector<stats::Metric>& ms,
                                               std::vector<stats::Metric>* figures) {
    std::vector<Element> out;
    out.reserve(ms.size() * 2);
    // How many metrics will actually draw here — the ones with a series.
    std::size_t drawn = 0;
    for (const auto& mt : ms) if (!mt.series.empty()) ++drawn;

    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];

        if (mt.series.empty()) {
            if (figures) figures->push_back(mt);
            else {
                if (!out.empty()) out.push_back(blank());
                out.push_back(text(stats::format(mt.unit, mt.value),
                                   fg_bold(series_hue(0, i))));
                if (!mt.label.empty()) out.push_back(text(mt.label, fg_dim(hue::dim)));
            }
            continue;
        }

        std::vector<float> series;
        series.reserve(mt.series.size());
        for (double d : mt.series) series.push_back(static_cast<float>(d));
        // A RATE over turns is a curve, not a strip. A spark is one cell per
        // sample, so a nine-turn history drew nine blocks — and with a steady
        // ~93% every block was full height, which reads as a solid slab
        // rather than a trend. The braille plot fills the card, draws a
        // connected line, and carries an axis, so the shape and the level are
        // both legible.
        if (mt.unit == stats::Unit::Ratio) {
            // Share the body between the charts that will actually draw.
            const int n = static_cast<int>(std::max<std::size_t>(1, drawn));
            const int h = std::clamp((panel_detail::panel_viewport_h() - 4) / n - 1,
                                     4, 14);
            maya::LineChart chart{std::move(series), h};
            chart.set_color(series_hue(0, i));
            if (!out.empty()) out.push_back(blank());
            out.push_back(chart.build());
            std::string cap = mt.label;
            if (mt.value != 0) cap += "  " + stats::format(mt.unit, mt.value);
            out.push_back(text(cap, fg_dim(hue::dim)));
            continue;
        }

        maya::SparklineConfig scfg{.color = series_hue(0, i), .show_last = false};
        maya::Sparkline spark{std::move(series), scfg};
        // The value rides in the LABEL so it sits on the trace's line.
        std::string label = mt.label;
        if (mt.value != 0 || mt.of != 0)
            label += "  " + stats::format(mt.unit, mt.value);
        spark.set_label(label);
        // Claim the column. A component reports a natural width and flex
        // leaves it there, so a nine-point trace sat as a stub in a column
        // three times its width while the histogram beside it filled the
        // same space. grow() hands it the slack, and the widget stretches
        // each sample across the cells it gets.
        out.push_back(spark.build() | dsl::grow(1.0f));
    }
    return out;
}

// Plot — a braille line chart per metric from its series, sized to the
// height the panel has rather than a fixed 6 rows: one plot is usually the
// whole card, so the rows it does not take are dead space in the column.
[[nodiscard]] std::vector<Element> build_plots(const std::vector<stats::Metric>& ms) {
    std::vector<Element> out;
    out.reserve(ms.size() * 2);
    // Share the body between plots when a section carries several, and keep
    // room for the card's heading, its blank and each plot's caption.
    const int n = std::max<int>(1, static_cast<int>(ms.size()));
    const int avail = (panel_detail::panel_viewport_h() - 3) / n - 1;
    const int height = std::clamp(avail, 4, 20);
    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];
        std::vector<float> series;
        series.reserve(mt.series.size());
        for (double d : mt.series) series.push_back(static_cast<float>(d));
        maya::LineChart chart{std::move(series), height};
        chart.set_color(series_hue(0, i));
        out.push_back(chart.build());
        if (!mt.label.empty()) out.push_back(text(mt.label, fg_dim(hue::dim)));
    }
    return out;
}

// Band — was a stacked bar of one metric's parts; the standalone equivalent
// is a BarChart of those parts, so each segment reads as its own labelled bar
// against the whole.
[[nodiscard]] std::vector<Element> build_band(const std::vector<stats::Metric>& ms) {
    std::vector<maya::Bar> bars;
    float peak = 0.0f;
    for (const auto& mt : ms) {
        for (std::size_t i = 0; i < mt.parts.size(); ++i) {
            const auto& p = mt.parts[i];
            const float v = static_cast<float>(p.value);
            peak = std::max(peak, v);
            bars.push_back({.label = p.label,
                            .value = v,
                            .color = series_hue(p.hue, i)});
        }
    }
    std::vector<Element> out;
    if (bars.empty()) { out.push_back(empty_placeholder()); return out; }
    out.push_back(maya::BarChart{std::move(bars), peak}.build());
    return out;
}

// Donut — the parts of a whole as a braille ring, one segment per part. The
// metric's detail sits in the centre (the headline share), its label as the
// caption. One donut per metric, since a donut IS a composition.
[[nodiscard]] std::vector<Element> build_donuts(const std::vector<stats::Metric>& ms) {
    std::vector<Element> out;
    for (const auto& mt : ms) {
        if (mt.parts.empty()) continue;
        maya::Donut d;
        for (std::size_t i = 0; i < mt.parts.size(); ++i) {
            const auto& p = mt.parts[i];
            d.segment(p.label, p.value, series_hue(p.hue, i));
        }
        if (!mt.detail.empty()) d.center(mt.detail);
        d.caption(mt.label);
        // Fill the height the panel actually has rather than a fixed 7 rows.
        // The body is panel_viewport_h() tall; take off the caption, the
        // card's heading and its blank, and a row of slack so the ring never
        // pushes the card into the scroll. One donut is the whole card, so
        // the height it does not use is dead space in the column.
        const int avail = panel_detail::panel_viewport_h() - 4;
        d.rows(std::clamp(avail, 5, 16));
        out.push_back(d.build());
    }
    if (out.empty()) out.push_back(empty_placeholder());
    return out;
}

// Hist / Dist — a distribution as block columns, one bucket per metric.
//
// No caption: the CARD's heading already names the section, and the
// histogram repeating it printed "First byte spread" twice, one line under
// the other. The card owns the title; the chart just draws.
[[nodiscard]] std::vector<Element> build_hist(
    std::string_view /*heading*/, const std::vector<stats::Metric>& ms) {
    maya::Histogram h;
    for (const auto& mt : ms) h.bucket(mt.label, mt.value);
    h.rows(6);
    std::vector<Element> out;
    out.push_back(h.build());
    return out;
}

// The width a card may lay itself out in.
//
// Panel measures a PREBUILT body element against an unbounded probe (1<<14)
// to discover its height — it only runs the column flow for `items`. A card
// holding a self-sizing chart (BarChart fills its slot, then prints the
// value at the far end) answers that probe honestly: it sizes a row to
// sixteen thousand columns, and when the card is then PAINTED in the real
// ~36-column body the value is the part that falls off the edge. That is
// the "189 clipped writes" the golden test catches.
//
// So the card states its own ceiling. terminal_cols() is the width the
// frame will really be painted at; Config::content_width() takes off the
// border and padding, and the scrollbar gutter comes off too because
// build() puts the body and the gutter side by side.
// The width the card GRID is laid out in — the real body width, so
// viewport() divides a true number into columns and the unbounded measure
// probe cannot inflate a self-sizing chart past the slot it paints in.
[[nodiscard]] int body_width() {
    const int frame = maya::panel::detail::terminal_cols() > 0
                          ? maya::panel::detail::terminal_cols()
                          : 80;
    // Frame + padding + the scrollbar gutter come off via content_width and
    // kScrollbarCols. card() then pads each cell one cell either side, and
    // THAT is what the grid must also leave room for — without it the last
    // column's content runs under the gutter by exactly those two cells.
    constexpr int kCardPadCols = 2;
    const int inner = maya::panel::Config::content_width(frame)
                      - maya::panel::Config::kScrollbarCols
                      - kCardPadCols;
    return std::max(16, inner);
}

// One section → one or MANY cards.
//
// A Kv/Hero section is a LIST of independent figures, and the example's
// dashboard is a grid of small cards — one figure each — which is what
// gives viewport() enough cells to fan into three and four columns. Folding
// a whole section's figures into one tall card leaves the grid two wide
// with half the screen empty, which is exactly how this looked against the
// example. So the scalar kinds SPLIT: every metric becomes its own card,
// titled with its label and showing its value.
//
// The picture kinds (Bars/Spark/Plot/Band/Donut/Hist) get a card each —
// their metrics are SERIES OF ONE chart, not separate figures.
//
// The scalar kinds do NOT get a card each. Each scalar SECTION becomes a
// group of lines appended to `text_groups`; the caller then deals those
// groups into a couple of text columns, so a tab is a picture plus two
// readout columns rather than one tall column beside a chart.
void build_cards(const stats::Section& sec, const stats::Facts& f,
                 std::vector<stats::Metric>& scratch,
                 std::vector<std::vector<Element>>& text_groups,
                 std::vector<Element>& out,
                 std::vector<bool>& tall,
                 std::vector<std::string>& titles) {
    scratch.clear();
    sec.extract(f, scratch);

    const std::size_t slot = out.size();
    const maya::Color accent = series_hue(0, slot);

    // ── scalar kinds: one GROUP of lines, dealt into a column later ───
    if (sec.viz == stats::Viz::Kv || sec.viz == stats::Viz::Hero) {
        if (scratch.empty()) return;
        std::vector<Element> group;
        group.reserve(scratch.size() * 4 + 2);
        if (!sec.heading.empty()) {
            group.push_back(text(std::string{sec.heading}, fg_bold(accent)));
            group.push_back(blank());
        }
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            const auto& mt = scratch[i];
            group.push_back(text(stats::format(mt.unit, mt.value), fg_bold(accent)));
            if (!mt.label.empty())  group.push_back(text(mt.label, fg_dim(hue::dim)));
            if (!mt.detail.empty()) group.push_back(text(mt.detail, fg_dim(hue::dim)));
            if (i + 1 < scratch.size()) group.push_back(blank());
        }
        text_groups.push_back(std::move(group));
        return;
    }

    if (scratch.empty()) {
        out.push_back(card(sec.heading, accent, {empty_placeholder()}));
        tall.push_back(false);
        titles.push_back(std::string{sec.heading});
        return;
    }

    // ── picture kinds: one card for the whole section ──────────────────
    //
    // A Spark section can also carry plain figures (no series). Those go to
    // the readout column rather than under the chart — there is room for
    // them there, and the chart keeps its height.
    std::vector<stats::Metric> figures;

    // A Spark section emits ONE CARD PER TRACE, not one card holding them
    // all. Each trace then pairs with the distribution of the same
    // quantity: the Stream tab reads as "first byte, and its spread" beside
    // "output rate, and its spread" rather than both traces in one column
    // and both spreads in another, which asks the reader to carry a number
    // across the panel to meet its own histogram.
    if (sec.viz == stats::Viz::Spark) {
        for (const auto& mt : scratch) {
            if (mt.series.empty()) { figures.push_back(mt); continue; }
            std::vector<stats::Metric> one{mt};
            auto one_body = build_sparks(one, nullptr);
            if (one_body.empty()) continue;
            out.push_back(card(mt.label, accent, std::move(one_body)));
            tall.push_back(false);
            titles.push_back(mt.label);
        }
        if (!figures.empty()) {
            std::vector<Element> group;
            group.reserve(figures.size() * 4);
            for (std::size_t i = 0; i < figures.size(); ++i) {
                const auto& mt = figures[i];
                group.push_back(text(stats::format(mt.unit, mt.value), fg_bold(accent)));
                if (!mt.label.empty())  group.push_back(text(mt.label, fg_dim(hue::dim)));
                if (!mt.detail.empty()) group.push_back(text(mt.detail, fg_dim(hue::dim)));
                if (i + 1 < figures.size()) group.push_back(blank());
            }
            text_groups.push_back(std::move(group));
        }
        return;
    }

    std::vector<Element> body;
    switch (sec.viz) {
        case stats::Viz::Bars:  body = build_bars(scratch);               break;
        case stats::Viz::Spark: body = {};                                break;
        case stats::Viz::Plot:  body = build_plots(scratch);              break;
        case stats::Viz::Band:  body = build_band(scratch);               break;
        case stats::Viz::Donut: body = build_donuts(scratch);             break;
        case stats::Viz::Hist:
        case stats::Viz::Dist:  body = build_hist(sec.heading, scratch);  break;
        case stats::Viz::Kv:
        case stats::Viz::Hero:  break;   // handled above
    }

    if (!figures.empty()) {
        std::vector<Element> group;
        group.reserve(figures.size() * 4);
        for (std::size_t i = 0; i < figures.size(); ++i) {
            const auto& mt = figures[i];
            group.push_back(text(stats::format(mt.unit, mt.value), fg_bold(accent)));
            if (!mt.label.empty())  group.push_back(text(mt.label, fg_dim(hue::dim)));
            if (!mt.detail.empty()) group.push_back(text(mt.detail, fg_dim(hue::dim)));
            if (i + 1 < figures.size()) group.push_back(blank());
        }
        text_groups.push_back(std::move(group));
    }

    // Every metric was a figure — nothing left to draw, so no card.
    if (body.empty() && !figures.empty()) return;

    if (body.empty()) body.push_back(empty_placeholder());
    out.push_back(card(sec.heading, accent, std::move(body)));
    tall.push_back(sec.viz == stats::Viz::Donut);
    titles.push_back(std::string{sec.heading});
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
    cfg.accent   = hue::cyan;

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

    // The panel body: one card per section, fanned into responsive columns
    // by maya::viewport() — the same layout the agent_stats example is built
    // around. viewport() picks the column count from a width CEILING (a slot
    // at or under max_width is one column, over it two, over twice it
    // three…), fills left-to-right then wraps, and keeps every card
    // responsive to its OWN column rather than the screen.
    //
    // The whole grid goes in as ONE prebuilt Element. Panel only runs its
    // own column flow for `items`, and it measures a prebuilt body against
    // an unbounded probe (1<<14) — so the grid is handed the REAL width via
    // .width, which is both what makes the columns come out right and what
    // stops a self-sizing chart from sizing itself to sixteen thousand
    // columns and then losing its tail when painted in the real body.
    std::vector<stats::Metric> scratch;
    scratch.reserve(32);

    const auto& sections = stats::tab_desc(active).sections;
    std::vector<std::vector<Element>> text_groups;   // one per scalar section
    std::vector<Element> pictures;                   // one card per chart
    text_groups.reserve(sections.size());
    pictures.reserve(sections.size());
    std::vector<bool> tall_picture;
    std::vector<std::string> picture_title;
    for (const auto& sec : sections)
        build_cards(sec, f, scratch, text_groups, pictures, tall_picture, picture_title);

    // Past two charts, share a cell — but only with one that FITS.
    //
    // Every chart as its own cell makes a chart-heavy tab too many columns
    // wide, and a cell that wraps lands on a grid row that starts below the
    // body: a section the reader never sees. A spark and a histogram share
    // a column comfortably.
    //
    // Two DONUTS do not — a dozen rows each against a fourteen-row body —
    // and pairing them blind to height is what cost the Tools tab "calls by
    // outcome". A chart joins the open cell only while the running height
    // leaves room; otherwise it starts its own.
    //
    // NOMINAL heights, not a live measure: sizing the pack from the terminal
    // makes one thread group its charts differently between renders.
    if (pictures.size() > 2) {
        // A cell may run TALLER than the body: a paired trace-and-spread is
        // the thing worth seeing together, and the second half of the pair
        // being a scroll away still beats it being in another column with a
        // number to carry across the panel. Two short charts fit; a donut
        // still refuses to share, since two of those is twice the body.
        constexpr int kCellRows  = 16;
        constexpr int kDonutRows = 12;
        constexpr int kShortRows = 7;

        // Pair a trace with the SPREAD OF THE SAME QUANTITY first.
        //
        // Packing in section order puts both traces in one cell and both
        // spreads in the next, so the reader carries "first byte 341ms"
        // across the panel to meet its own histogram. Matching on the
        // subject word keeps each story in one column: first byte and its
        // spread, then output rate and its spread.
        std::vector<std::size_t> order(pictures.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::vector<bool> taken(pictures.size(), false);
        std::vector<std::size_t> seq;
        for (std::size_t i = 0; i < pictures.size(); ++i) {
            if (taken[i]) continue;
            taken[i] = true;
            seq.push_back(i);
            // The partner is the next unpaired card whose title shares this
            // one's leading word ("First byte" -> "First byte spread").
            const std::string& mine = picture_title[i];
            const std::size_t cut = mine.find(' ');
            const std::string key = cut == std::string::npos ? mine : mine.substr(0, cut);
            if (key.empty()) continue;
            for (std::size_t k = i + 1; k < pictures.size(); ++k) {
                if (taken[k]) continue;
                if (picture_title[k].rfind(key, 0) == 0) { taken[k] = true; seq.push_back(k); break; }
            }
        }

        std::vector<Element> packed;
        std::vector<Element> cur;
        int cur_rows = 0;
        for (std::size_t idx : seq) {
            const int h = (idx < tall_picture.size() && tall_picture[idx])
                              ? kDonutRows : kShortRows;
            if (!cur.empty() && cur_rows + 1 + h > kCellRows) {
                packed.push_back(dsl::v(std::move(cur)).build());
                cur.clear();
                cur_rows = 0;
            }
            if (!cur.empty()) { cur.push_back(blank()); ++cur_rows; }
            cur_rows += h;
            cur.push_back(std::move(pictures[idx]));
        }
        if (!cur.empty()) packed.push_back(dsl::v(std::move(cur)).build());
        pictures = std::move(packed);
    }

    // Deal the scalar figures into TWO columns, balanced by LINE COUNT, so
    // a tab is a readout plus its pictures — cells that divide a wide panel
    // evenly — rather than one very tall column beside a chart.
    //
    // Balancing by line rather than by group, because dealing whole groups
    // only balances when the groups are of similar size: the Reasoning tab's
    // are three lines against twelve, so whichever column took the big one
    // was four times the other and the figures ran off the bottom.
    //
    // The cut lands on a BLANK, which is the only place a figure ends.
    // Cutting mid-figure orphaned a detail ("16%") at the top of the second
    // column, away from the value it qualifies.
    std::vector<Element> text_a, text_b;
    if (!text_groups.empty()) {
        std::vector<Element> all;
        for (auto& g : text_groups) {
            if (!all.empty()) all.push_back(blank());
            for (auto& e : g) all.push_back(std::move(e));
        }

        const std::size_t half = all.size() / 2;
        std::size_t cut = all.size();
        for (std::size_t i = half; i < all.size(); ++i) {
            const auto* t = maya::as_text(all[i]);
            if (t && t->content.empty()) { cut = i; break; }
        }
        if (cut >= all.size()) {
            // No blank at or after the midpoint — look backwards instead.
            for (std::size_t i = half; i-- > 0;) {
                const auto* t = maya::as_text(all[i]);
                if (t && t->content.empty()) { cut = i; break; }
            }
        }
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i == cut) continue;            // drop the blank we cut on
            (i < cut ? text_a : text_b).push_back(std::move(all[i]));
        }
    }
    // The readout is its OWN two-column grid, nested inside one outer cell.
    //
    // Flattening picture + text_a + text_b into three sibling cells makes
    // the chart just one column of three, so it shrinks as the readout
    // grows. Nesting keeps the split at the top level to TWO — the readout,
    // and the pictures beside it — and lets the readout divide its own half
    // again.
    //
    // The inner grid is HANDED its width. It cannot see the outer split, so
    // measured against the unbounded probe it either collapses back to one
    // column (which is what kept the Reasoning tab's figures in a single
    // tall stack) or divides a width it does not have and clips.
    Element readout;
    bool has_readout = false;
    {
        std::vector<Element> text_cards;
        if (!text_a.empty()) text_cards.push_back(card({}, hue::cyan, std::move(text_a)));
        if (!text_b.empty()) text_cards.push_back(card({}, hue::cyan, std::move(text_b)));
        if (!text_cards.empty()) {
            const int n = static_cast<int>(text_cards.size());
            // The readout is the LEAD cell and holds two columns of text, so
            // it takes half the body — the charts divide the rest. Splitting
            // the width evenly between every outer cell left the figures
            // 20 columns each and wrapped their labels mid-phrase ("of
            // generated / tokens were / reasoning").
            const int share = std::max(34, body_width() / 2);
            readout = maya::viewport(std::move(text_cards),
                                     maya::ViewportOpts{.max_width = std::max(16, share / n),
                                                        .max_cols  = n,
                                                        .min_width = 14,
                                                        .gap       = 2,
                                                        .gap_y     = 2,
                                                        .width     = share,
                                                        .flow      = maya::Flow::Row})
                          .build();
            has_readout = true;
        }
    }

    // The readout LEADS — the figures are what must be readable first, and
    // the charts illustrate them.
    std::vector<Element> cards;
    cards.reserve(pictures.size() + 1);
    if (has_readout) cards.push_back(std::move(readout));
    for (auto& p : pictures) cards.push_back(std::move(p));

    const int ncards = static_cast<int>(cards.size());
    cfg.prebuilt.push_back(
        maya::viewport(std::move(cards),
                       maya::ViewportOpts{// A tab is usually the readout plus
                                          // one or two pictures, so let a
                                          // column get wide.
                                          .max_width = 56,
                                          // Never ask for more columns than
                                          // there are cards. Without this the
                                          // width can afford THREE columns
                                          // while the tab has two cards, and
                                          // viewport treats that as underfull
                                          // — it holds both at max_width and
                                          // leaves the surplus empty, which is
                                          // the dead strip on the right. Capped
                                          // at the card count the grid is full,
                                          // so it divides the whole slot
                                          // exactly and the cards FILL it.
                                          .max_cols  = ncards,
                                          // A chart still needs its label, a
                                          // bar and the value; below this the
                                          // grid stays one column rather than
                                          // truncating in two.
                                          .min_width = 30,
                                          .gap       = 3,
                                          .gap_y     = 2,
                                          .width     = body_width(),
                                          .flow      = maya::Flow::Row})
            .build());

    // No row selection: this is a document, not a picker.
    cfg.selected = -1;

    // Flow cards into responsive columns. A card needs ~30 columns to keep
    // its labels and values whole; capped at 40 so a very wide terminal
    // does not stretch a two-value tile across half the screen. The gap
    // keeps neighbouring cards from reading as one.
    // NOTE: col_max_width/col_min_width/col_gap are NOT set. Panel only runs
    // its column flow for `items`; a `prebuilt` body is one opaque stack —
    // here, the single viewport() grid above, which does the columns itself.

    // No rule off the headings. The accent heading and the column split
    // already separate the sections; a rule across every heading turns a
    // page of figures into a form.
    cfg.header_rule = false;

    cfg.scroll = &o->scroll;
    // Body height: the same one every other panel uses. A panel that matches
    // its siblings is worth more than one tuned to its own content; overflow
    // scrolls, which is what the scrollbar is for.
    cfg.viewport_h = panel_detail::panel_viewport_h();

    // Hold that height rather than shrink-wrapping to the tab. These tabs
    // differ in length, so without this the frame would resize every time
    // the reader pressed tab.
    cfg.fixed_viewport = true;

    cfg.note = visible.size() > 1 ? "tab  switch view   \xe2\x86\x91\xe2\x86\x93  scroll   esc  close"
                                  : "\xe2\x86\x91\xe2\x86\x93  scroll   esc  close";
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
