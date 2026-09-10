#pragma once
// agentty::stats — the tab table. THE SSOT for what the panel shows.
//
// `kTabs` is the single source for order, titles, subtitles, availability
// and count. Everything else is derived from it: tab_title(), tab_step(),
// kTabCount. The version this replaces had four parallel switch statements
// over one enum, which is four places to forget a case and four chances
// for them to disagree — the same argument that put ProviderDescriptor and
// kCommands on tables.
//
// Adding a tab is: an enumerator, a row here, and an extractor. The
// compiler names anything you forgot.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "agentty/domain/stats/facts.hpp"
#include "agentty/domain/stats/metric.hpp"

namespace agentty::stats {

enum class Tab : std::uint8_t {
    Session,
    Models,
    Smart,
    Tokens,
    Cache,
    Tools,
    Reasoning,
    Stream,
    Context,
    Retrieval,
};

struct TabDesc {
    Tab              id;
    std::string_view title;      // the strip chip
    std::string_view subtitle;   // the panel's subtitle line
    std::span<const Section> sections;
    // Hide a tab with nothing to say. A session that ran no tools should
    // not show a Tools tab explaining that it ran no tools — an empty
    // state is a tab admitting it should not have been drawn.
    bool (*available)(const Facts&);
};

// Sections per tab. Declared out-of-line so kTabs stays readable as a
// table; each `extract` is one function in extract.cpp.
namespace sections {
extern const std::array<Section, 4> session;
extern const std::array<Section, 2> models;
extern const std::array<Section, 3> smart;
extern const std::array<Section, 2> tokens;
extern const std::array<Section, 3> cache;
extern const std::array<Section, 4> tools;
extern const std::array<Section, 1> reasoning;
extern const std::array<Section, 5> stream;
extern const std::array<Section, 2> context;
extern const std::array<Section, 1> retrieval;
}  // namespace sections

inline constexpr std::array<TabDesc, 10> kTabs{{
    {Tab::Session,  "Session",   "what this thread actually did",
     sections::session,
     [](const Facts&) { return true; }},
    {Tab::Models,   "Models",    "which model served each turn",
     sections::models,
     [](const Facts& f) { return !f.models.by_model.empty(); }},
    {Tab::Smart,    "Smart",     "is routing sending work to cheaper models",
     sections::smart,
     [](const Facts& f) { return f.smart.routed > 0; }},
    {Tab::Tokens,   "Tokens",    "what this thread cost in tokens",
     sections::tokens,
     [](const Facts& f) { return f.session.measured_turns > 0; }},
    {Tab::Cache,    "Cache",     "how much of the prefix was served from cache",
     sections::cache,
     [](const Facts& f) { return f.cache.turns_with_cache > 0; }},
    {Tab::Tools,    "Tools",     "what the agent did, and how long it took",
     sections::tools,
     [](const Facts& f) { return f.tools.total > 0; }},
    {Tab::Reasoning, "Reasoning", "is extended thinking earning its keep",
     sections::reasoning,
     [](const Facts& f) { return f.reasoning.turns > 0; }},
    {Tab::Stream,   "Stream",    "transport health \xe2\x80\x94 retries, stalls, throughput",
     sections::stream,
     [](const Facts& f) { return f.session.measured_turns > 0; }},
    {Tab::Context,  "Context",   "how close this thread is to the window",
     sections::context,
     [](const Facts& f) { return f.context.peak_input > 0; }},
    {Tab::Retrieval, "Retrieval", "is proactive retrieval finding anything",
     sections::retrieval,
     [](const Facts& f) { return f.retrieval.injections > 0; }},
}};

inline constexpr int kTabCount = static_cast<int>(kTabs.size());

[[nodiscard]] constexpr const TabDesc& tab_desc(Tab t) noexcept {
    for (const auto& d : kTabs) if (d.id == t) return d;
    return kTabs[0];
}
[[nodiscard]] constexpr std::string_view tab_title(Tab t) noexcept {
    return tab_desc(t).title;
}
[[nodiscard]] constexpr std::string_view tab_subtitle(Tab t) noexcept {
    return tab_desc(t).subtitle;
}
[[nodiscard]] inline bool tab_available(Tab t, const Facts& f) {
    const auto& d = tab_desc(t);
    return d.available && d.available(f);
}

// The visible tabs, in table order. The panel renders exactly this.
[[nodiscard]] inline std::vector<Tab> visible_tabs(const Facts& f) {
    std::vector<Tab> out;
    out.reserve(kTabs.size());
    for (const auto& d : kTabs)
        if (d.available && d.available(f)) out.push_back(d.id);
    if (out.empty()) out.push_back(Tab::Session);   // Session is always available
    return out;
}

// Step within the VISIBLE set, so Tab/Shift-Tab can never land on a tab
// that is not drawn. Wraps in both directions.
[[nodiscard]] inline Tab tab_step(Tab cur, int delta, const Facts& f) {
    const auto vis = visible_tabs(f);
    const int n = static_cast<int>(vis.size());
    if (n == 0) return Tab::Session;
    int i = 0;
    for (int k = 0; k < n; ++k) if (vis[static_cast<std::size_t>(k)] == cur) { i = k; break; }
    int next = (i + delta) % n;
    if (next < 0) next += n;
    return vis[static_cast<std::size_t>(next)];
}

}  // namespace agentty::stats
