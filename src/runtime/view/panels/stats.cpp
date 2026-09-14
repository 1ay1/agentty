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

// The card accent for the active tab. Kept fixed rather than per-tab: the
// panel already tells you which tab you are on via the strip, so tinting
// the cards a different colour per tab would be a second, redundant, and
// slower-to-read signal.
[[nodiscard]] maya::Color card_accent() { return maya::Color::rgb(120, 180, 255); }

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
[[nodiscard]] maya::Color series_hue(int slot, std::size_t i) {
    // The catch-all slice draws muted. "other" is the ABSENCE of a
    // category, so giving it a category's colour makes it read as one
    // more of them — which it did: it collided with the largest slice.
    if (slot < 0) return muted;
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

// ── card() — a bare titled section, matching the agent_stats look ─────────
//
// No border: the accent-coloured heading over the content IS the section
// boundary. A box around every card turns a page of figures into a grid of
// forms, and in two columns the boxes double up along the gutter. The
// vstack + padding(0,1) gives the breathing room a border would have,
// without the ink.
[[nodiscard]] Element card(std::string_view heading, std::vector<Element> body) {
    std::vector<Element> rows;
    rows.reserve(body.size() + 3);
    if (!heading.empty()) {
        rows.push_back(text(std::string{heading}, fg_bold(card_accent())));
        rows.push_back(blank());
    }
    for (auto& e : body) rows.push_back(std::move(e));
    // Trailing blank: with no border, the gap between cards IS the
    // separator. Without it the next card's heading butts against this
    // card's last figure and the two read as one section.
    rows.push_back(blank());
    return (dsl::v(std::move(rows)) | padding(0, 1)).build();
}

// A section with nothing to say still draws its heading, so a tab does not
// silently drop a section the reader expected — the dim dash says "measured,
// empty" rather than "forgot to render".
[[nodiscard]] Element empty_placeholder() {
    return text("\xe2\x80\x94", fg_dim(muted));   // — em dash
}

// ── Viz builders ──────────────────────────────────────────────────────────
//
// Each takes the extracted metrics and returns the card body (the elements
// under the heading). One function per PRESENTATION, so this file grows a
// case when a Viz is added, never when a tab is.

// Kv / Hero — stat tiles. A tile is one line, "value  label", so a column
// of them reads as a list of facts rather than a ladder of alternating
// bold/dim rows the eye has to re-pair. Hero promotes the FIRST metric to
// a headline: value on its own line with the label beneath it.
[[nodiscard]] std::vector<Element> build_tiles(
    const std::vector<stats::Metric>& ms, bool hero) {
    std::vector<Element> out;
    out.reserve(ms.size() * 3);
    for (std::size_t i = 0; i < ms.size(); ++i) {
        const auto& mt = ms[i];
        const std::string val = stats::format(mt.unit, mt.value);
        const bool headline = hero && i == 0;

        if (headline) {
            // The lead figure of a Hero tab is the headline: big value on
            // its own line, label beneath it.
            out.push_back(text(val, fg_bold(card_accent())));
            if (!mt.label.empty()) out.push_back(text(mt.label, fg_dim(muted)));
            if (!mt.detail.empty()) out.push_back(text(mt.detail, fg_dim(muted)));
        } else {
            // A tile is one line: the accented value, then its label, so a
            // column of tiles reads as "number — what it is" rather than a
            // ladder of alternating bold/dim rows the eye has to re-pair.
            std::string line = val;
            std::vector<maya::StyledRun> runs;
            runs.push_back({0, line.size(),
                            maya::Style{}.with_fg(card_accent()).with_bold()});
            if (!mt.label.empty()) {
                const std::size_t at = line.size();
                line += "  " + mt.label;
                runs.push_back({at, line.size() - at, maya::Style{}.with_fg(muted)});
            }
            out.push_back(Element{maya::TextElement{.content = std::move(line),
                                                    .runs = std::move(runs)}});
            if (!mt.detail.empty()) out.push_back(text("  " + mt.detail, fg_dim(muted)));
        }
        // Space tiles apart so each number+label pair is its own block.
        if (i + 1 < ms.size()) out.push_back(blank());
    }
    return out;
}

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
        if (!mt.label.empty()) out.push_back(text(mt.label, fg_dim(muted)));
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
[[nodiscard]] int card_width() {
    const int frame = maya::panel::detail::terminal_cols() > 0
                          ? maya::panel::detail::terminal_cols()
                          : 80;
    const int inner = maya::panel::Config::content_width(frame)
                      - maya::panel::Config::kScrollbarCols;
    return std::max(16, inner);
}

// One section → one card. Runs the extractor, then dispatches on the Viz to
// the builder that knows which fields the extractor filled.
[[nodiscard]] Element build_card(const stats::Section& sec, const stats::Facts& f,
                                 std::vector<stats::Metric>& scratch) {
    scratch.clear();
    sec.extract(f, scratch);

    Element el = [&] {
        if (scratch.empty())
            return card(sec.heading, {empty_placeholder()});

        std::vector<Element> body;
        switch (sec.viz) {
            case stats::Viz::Kv:    body = build_tiles(scratch, /*hero=*/false); break;
            case stats::Viz::Hero:  body = build_tiles(scratch, /*hero=*/true);  break;
            case stats::Viz::Bars:  body = build_bars(scratch);                  break;
            case stats::Viz::Spark: body = build_sparks(scratch);                break;
            case stats::Viz::Plot:  body = build_plots(scratch);                 break;
            case stats::Viz::Band:  body = build_band(scratch);                  break;
            case stats::Viz::Donut: body = build_donuts(scratch);               break;
            case stats::Viz::Hist:
            case stats::Viz::Dist:  body = build_hist(sec.heading, scratch);     break;
        }
        if (body.empty()) body.push_back(empty_placeholder());
        return card(sec.heading, std::move(body));
    }();

    // Pin the card to the real body width. max_width alone was not enough:
    // a child component's own measure can still answer the unbounded probe
    // with a bigger number, and the parent honours it. A FIXED width is the
    // offer its children are measured against, so measure and paint agree
    // and a self-sizing chart sizes to the slot it will be painted in.
    if (auto* bx = maya::as_box(el)) {
        const int w = card_width();
        bx->layout.width     = maya::Dimension::fixed(w);
        bx->layout.max_width = maya::Dimension::fixed(w);
    }
    return el;
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
    cfg.accent   = card_accent();

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

    // The panel body: one card per section, flowed into columns. Each card
    // is a prebuilt Element — Panel accounts for its own frame (border,
    // padding, scrollbar gutter) before laying the body out, so a card is
    // measured against a width it may actually paint in.
    std::vector<stats::Metric> scratch;
    scratch.reserve(32);

    const auto& sections = stats::tab_desc(active).sections;
    cfg.prebuilt.reserve(sections.size());
    for (const auto& sec : sections)
        cfg.prebuilt.push_back(build_card(sec, f, scratch));

    // No row selection: this is a document, not a picker.
    cfg.selected = -1;

    // Flow cards into responsive columns. A card needs ~30 columns to keep
    // its labels and values whole; capped at 40 so a very wide terminal
    // does not stretch a two-value tile across half the screen. The gap
    // keeps neighbouring cards from reading as one.
    // NOTE: col_max_width/col_min_width/col_gap are NOT set. Panel only runs
    // its column flow for `items`; a `prebuilt` body is one opaque stack. The
    // cards cap their own width (card_width()) so the unbounded measure probe
    // cannot inflate a self-sizing chart past the slot it is painted in.

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
