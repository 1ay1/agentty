// stats.cpp — the stats viewer overlay.
//
// A tabbed, read-only projection of the session. Today one tab (Smart Mode);
// the tab strip and the dispatch are built for N from the start so a second
// tab is an enumerator plus an arm, not a refactor.
//
// ── Where the numbers come from ──────────────────────────────────────────
//
// stats::smart_stats() over the transcript. Nothing here counts anything —
// see domain/stats.hpp for why the projection is derived rather than
// accumulated. This file is presentation only: it turns Rows into bars.
//
// The one non-obvious thing it does is REFRESH THE PANEL'S CACHE (the stamp
// comparison below). That is deliberate and it is why the cache fields are
// `mutable`: the alternative is an O(turns) walk plus a map and a sort on
// every frame, on the same thread as the streaming reveal. See the note in
// panel/stats.hpp.

#include "agentty/runtime/view/panels.hpp"

#include "agentty/domain/stats.hpp"
#include "agentty/runtime/panel/stats.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "panels_common.hpp"

#include <maya/widget/panel.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// ── Bar geometry ─────────────────────────────────────────────────────────
// One width for every bar in the pane. A share is only readable against its
// neighbours if they all measure the same, so this is a constant rather than
// a per-row fit.
constexpr int kBarCells = 24;

// A proportional bar. Uses eighth-block glyphs so a share smaller than one
// cell is still VISIBLE — the whole point of the panel is spotting a role
// that got almost no work, and a bar that rounds to empty hides exactly
// that. A non-zero share therefore always paints at least the narrowest
// partial block.
[[nodiscard]] std::string bar_of(double share, int cells = kBarCells) {
    static constexpr const char* kEighths[] = {
        "", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
        "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89",
    };
    if (share <= 0.0) return {};
    const double exact = std::clamp(share, 0.0, 1.0) * cells;
    int full = static_cast<int>(exact);
    int rem  = static_cast<int>((exact - full) * 8.0);
    if (full == 0 && rem == 0) rem = 1;      // never round a real share away
    std::string s;
    s.reserve(static_cast<std::size_t>(full) * 3 + 3);
    for (int i = 0; i < full; ++i) s += "\xe2\x96\x88";   // █
    if (rem > 0 && full < cells) s += kEighths[rem];
    return s;
}

// "42%" — one integer, no decimals. A share is a glance, not a measurement;
// two decimals invite the reader to compare digits that the sample size does
// not support.
[[nodiscard]] std::string pct_of(double share) {
    return std::to_string(static_cast<int>(share * 100.0 + 0.5)) + "%";
}

// A tally row: label, bar, count and share, column-aligned.
//
// Aligned by PADDING rather than by a table widget: the panel body is a
// plain Element list, and a bar chart's readability comes from its left
// edges lining up, which padding gives directly.
[[nodiscard]] Element tally_row(const stats::Row& r, int label_w,
                               Color bar_color, bool dim_when_zero) {
    // Pad to the widest label PLUS a fixed gutter. Padding to the widest
    // alone leaves the longest row with zero space — "Implementation" ran
    // straight into its own bar while every shorter row had a gap, which
    // read as a rendering fault rather than as alignment.
    constexpr int kGutter = 2;
    const int col = label_w + kGutter;
    std::string label = r.label;
    if (static_cast<int>(label.size()) > col)
        label = label.substr(0, static_cast<std::size_t>(col));
    label.append(static_cast<std::size_t>(col) - label.size(), ' ');

    const bool zero = r.count == 0;
    auto label_style = zero && dim_when_zero ? fg_dim(muted) : fg_of(fg);

    std::string tail = "  " + std::to_string(r.count);
    // A zero row says so in words. "0" alone reads as a rendering failure;
    // "0 turns" is a finding.
    tail += r.count == 1 ? " turn" : " turns";
    tail += "  \xc2\xb7  " + pct_of(r.share);

    return h(text(label, label_style),
             text(bar_of(r.share), fg_of(bar_color)),
             text(tail, fg_dim(muted))).build();
}

// A section heading inside the body.
[[nodiscard]] Element section(std::string_view title) {
    return text(std::string{title}, fg_dim(muted)).build();
}

[[nodiscard]] Element blank_row() { return text("").build(); }

// ── The Smart Mode tab ───────────────────────────────────────────────────
//
// Reads top-down as an answer to "is Smart Mode working?": the headline
// first, then the role split that justifies it, then the models that
// actually served turns.
[[nodiscard]] std::vector<Element> smart_tab(const stats::SmartStats& s) {
    std::vector<Element> rows;

    if (s.empty()) {
        // An empty state, not a wall of zeroes. Zeroes here would read as
        // "Smart Mode is broken" when they only mean "no turns yet".
        rows.push_back(text("No turns in this thread yet.", fg_of(fg)).build());
        rows.push_back(blank_row());
        rows.push_back(text("Ask something and this fills in — it reports "
                            "which model served each turn.",
                            fg_dim(muted)).build());
        return rows;
    }

    if (s.routed_turns == 0) {
        rows.push_back(text("Smart Mode was off for every turn in this thread.",
                            fg_of(fg)).build());
        rows.push_back(blank_row());
        rows.push_back(
            text("Turn it on with ^S to route each turn to the cheapest "
                 "model that can do it.", fg_dim(muted)).build());
        return rows;
    }

    // ── Headline ─────────────────────────────────────
    // Delegated share is the number that was structurally 0% before the main
    // turn was routed by complexity, so it is the direct answer to "is it
    // working?". Coloured by outcome: green when work is leaving the
    // flagship, amber when none is.
    const bool delegating = s.delegated_share > 0.0;
    rows.push_back(h(
        text(pct_of(s.delegated_share),
             fg_bold(delegating ? success : warn)),
        text(" of routed turns ran BELOW the Strategic model", fg_of(fg))
    ).build());
    rows.push_back(blank_row());

    // ── By role ──────────────────────────────────────
    rows.push_back(section("BY ROLE"));
    int label_w = 0;
    for (const auto& r : s.by_role)
        label_w = std::max(label_w, static_cast<int>(r.label.size()));
    for (const auto& r : s.by_role)
        rows.push_back(tally_row(r, label_w, accent, /*dim_when_zero=*/true));

    rows.push_back(blank_row());

    // ── By model ────────────────────────────────────
    // The row a user checks against their provider's billing page, so it
    // shows the id as dispatched (pretty label, but never a renamed model).
    rows.push_back(section("BY MODEL"));
    int mlabel_w = 0;
    std::vector<stats::Row> pretty;
    pretty.reserve(s.by_model.size());
    for (const auto& r : s.by_model) {
        stats::Row p = r;
        std::string label = pretty_model_label(r.label);
        if (!label.empty()) p.label = std::move(label);
        mlabel_w = std::max(mlabel_w, static_cast<int>(p.label.size()));
        pretty.push_back(std::move(p));
    }
    for (const auto& r : pretty)
        rows.push_back(tally_row(r, mlabel_w, info, /*dim_when_zero=*/false));

    // ── Denominator ─────────────────────────────────
    // Always state what the percentages are OF. A share without its
    // denominator is the classic way a stats panel misleads honestly.
    rows.push_back(blank_row());
    std::string denom = std::to_string(s.routed_turns) + " routed";
    if (s.unrouted_turns > 0)
        denom += "  \xc2\xb7  " + std::to_string(s.unrouted_turns) +
                 " ran with Smart Mode off";
    denom += "  \xc2\xb7  " + std::to_string(s.total_turns) + " total";
    rows.push_back(text(std::move(denom), fg_dim(muted)).build());

    return rows;
}

}  // namespace

Element stats_panel(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Stats>();
    if (!o) return nothing();

    // Refresh the cached projection when the transcript has moved. See
    // panel/stats.hpp: the cache is an optimisation over a pure function,
    // never a second source of truth.
    stats_panel::Stamp now{m.d.current.messages.size(), m.d.current.id.value};
    if (!(o->stamp == now)) {
        o->smart = stats::smart_stats(m.d.current.messages);
        o->stamp = std::move(now);
    }

    maya::panel::Config cfg;
    cfg.title    = "Stats";
    cfg.subtitle = std::string{stats::tab_subtitle(o->tab)};
    cfg.accent   = accent;

    // Tabs are the WIDGET's chrome, not this host's: it owns the padding,
    // the selected treatment and how the strip degrades on a narrow frame,
    // so every tabbed panel looks the same by construction. This host only
    // says which tabs exist and which one is live — both read straight off
    // the stats::Tab enumeration, which is their SSOT.
    cfg.tabs.reserve(static_cast<std::size_t>(stats::kTabCount));
    for (int i = 0; i < stats::kTabCount; ++i)
        cfg.tabs.emplace_back(stats::tab_title(static_cast<stats::Tab>(i)));
    cfg.tab_active = static_cast<int>(o->tab);

    // Tab dispatch. A switch on the enum rather than a table of function
    // pointers: -Wswitch then names a new tab that forgot its body, which is
    // the same guarantee the panel Kind dispatch relies on.
    switch (o->tab) {
        case stats::Tab::Smart:
            cfg.prebuilt = smart_tab(o->smart);
            break;
    }

    // Read-only: no cursor. A selection highlight on rows nothing can be
    // done to is a promise the panel cannot keep.
    cfg.selected = -1;
    cfg.scroll     = &m.ui.stats_scroll;
    cfg.viewport_h = panel_detail::panel_viewport_h();

    cfg.note = stats::kTabCount > 1 ? "tab  switch view   esc  close"
                                    : "esc  close";
    return maya::Panel{std::move(cfg)}.build();
}

}  // namespace agentty::ui
