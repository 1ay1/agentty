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
[[nodiscard]] std::vector<Element> build_sparks(const std::vector<stats::Metric>& ms) {
    std::vector<Element> out;
    out.reserve(ms.size());
    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];
        std::vector<float> series;
        series.reserve(mt.series.size());
        for (double d : mt.series) series.push_back(static_cast<float>(d));
        maya::Sparkline spark{std::move(series),
                              {.color = series_hue(0, i), .show_last = false}};
        // The value rides in the LABEL so it sits on the trace's line.
        std::string label = mt.label;
        if (mt.value != 0 || mt.of != 0)
            label += "  " + stats::format(mt.unit, mt.value);
        spark.set_label(label);
        out.push_back(spark.build());
    }
    return out;
}

// Plot — a braille line chart per metric from its series. Height 6 is tall
// enough to show a trend's shape without dominating the card.
[[nodiscard]] std::vector<Element> build_plots(const std::vector<stats::Metric>& ms) {
    std::vector<Element> out;
    out.reserve(ms.size() * 2);
    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];
        std::vector<float> series;
        series.reserve(mt.series.size());
        for (double d : mt.series) series.push_back(static_cast<float>(d));
        maya::LineChart chart{std::move(series), 6};
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
        d.rows(7);
        out.push_back(d.build());
    }
    if (out.empty()) out.push_back(empty_placeholder());
    return out;
}

// Hist / Dist — a distribution as block columns, one bucket per metric. The
// section heading rides as the caption; six rows of height give the shape
// room to read.
[[nodiscard]] std::vector<Element> build_hist(
    std::string_view heading, const std::vector<stats::Metric>& ms) {
    maya::Histogram h;
    for (const auto& mt : ms) h.bucket(mt.label, mt.value);
    h.rows(6);
    if (!heading.empty()) h.caption(std::string{heading});
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
// The scalar kinds do NOT get a card each. Every figure a tab has is
// appended to ONE text column, handed in as `text_body`: a tab is then two
// or three cells — the readout, and a picture beside it — which is the
// shape that fills a wide panel instead of scattering four short cards
// across it with whitespace under each.
void build_cards(const stats::Section& sec, const stats::Facts& f,
                 std::vector<stats::Metric>& scratch,
                 std::vector<Element>& text_body,
                 std::vector<Element>& out) {
    scratch.clear();
    sec.extract(f, scratch);

    const std::size_t slot = out.size();
    const maya::Color accent = series_hue(0, slot);

    // ── scalar kinds: append to the shared text column ──────────────
    if (sec.viz == stats::Viz::Kv || sec.viz == stats::Viz::Hero) {
        if (scratch.empty()) return;
        // A heading between groups, so the figures of one section still
        // read as a group inside the shared column.
        if (!text_body.empty()) text_body.push_back(blank());
        if (!sec.heading.empty()) {
            text_body.push_back(text(std::string{sec.heading}, fg_bold(accent)));
            text_body.push_back(blank());
        }
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            const auto& mt = scratch[i];
            text_body.push_back(text(stats::format(mt.unit, mt.value), fg_bold(accent)));
            if (!mt.label.empty())  text_body.push_back(text(mt.label, fg_dim(hue::dim)));
            if (!mt.detail.empty()) text_body.push_back(text(mt.detail, fg_dim(hue::dim)));
            if (i + 1 < scratch.size()) text_body.push_back(blank());
        }
        return;
    }

    if (scratch.empty()) {
        out.push_back(card(sec.heading, accent, {empty_placeholder()}));
        return;
    }

    // ── picture kinds: one card for the whole section ──────────────────
    std::vector<Element> body;
    switch (sec.viz) {
        case stats::Viz::Bars:  body = build_bars(scratch);              break;
        case stats::Viz::Spark: body = build_sparks(scratch);            break;
        case stats::Viz::Plot:  body = build_plots(scratch);             break;
        case stats::Viz::Band:  body = build_band(scratch);              break;
        case stats::Viz::Donut: body = build_donuts(scratch);            break;
        case stats::Viz::Hist:
        case stats::Viz::Dist:  body = build_hist(sec.heading, scratch); break;
        case stats::Viz::Kv:
        case stats::Viz::Hero:  break;   // handled above
    }
    if (body.empty()) body.push_back(empty_placeholder());
    out.push_back(card(sec.heading, accent, std::move(body)));
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
    std::vector<Element> text_body;   // every scalar figure, one column
    std::vector<Element> pictures;    // one card per chart
    text_body.reserve(32);
    pictures.reserve(sections.size());
    for (const auto& sec : sections)
        build_cards(sec, f, scratch, text_body, pictures);

    // The picture leads and the readout follows — the chart is the thing
    // that carries the shape of the answer, so it gets the first column and
    // the numbers sit beside it.
    std::vector<Element> cards;
    cards.reserve(pictures.size() + 1);
    for (auto& p : pictures) cards.push_back(std::move(p));
    if (!text_body.empty())
        cards.push_back(card({}, hue::cyan, std::move(text_body)));

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
