#pragma once
// agentty::web_search_cfg — the user's policy for the `web_search` tool.
//
// ── What the host can and cannot decide ──────────────────────────────────
//
// web_search is implemented in mcp-cpp (third_party/mcp-cpp/src/tools/web.cpp):
// the engine chain, the HTML parsers, the region code and the dedup all live
// in that library, which is a separate repository. agentty cannot reach into
// it, and does not need to for anything below.
//
// What agentty DOES own are the two seams every web_search call passes
// through:
//
//   * advertisement — tools::select_wire_tools decides what goes on the wire
//   * dispatch      — the bridge's execute closure wraps every mcp-cpp tool
//
// So this policy is exactly the part that can be expressed at those seams:
// whether the tool exists at all, how many results a call asks for, and
// which sites a query must exclude. Engine choice and region need a seam in
// mcp-cpp first and are deliberately absent rather than half-wired.
//
// ── Why the arguments are rewritten, not the results filtered ────────────
//
// Filtering results after the fact would need the parsed hits, which only
// mcp-cpp has. Rewriting the request needs nothing but the args, and the
// engine then never returns a blocked domain, so there is nothing to filter —
// and it fills the freed slots with other results, which post-filtering could
// not do.
//
// What is and is not known about `-site:` support: DuckDuckGo documents it;
// Brave honours it (checked live: a query that returns ten pinterest.com hits
// returns none with `-site:pinterest.com`; Brave's own docs spell the operator
// `NOT site:`); Startpage is UNVERIFIED. The chain is failover, so Brave
// normally answers and Startpage only matters when the earlier two fail — in
// which case an exclusion may simply not apply. That is a limit of this
// approach, not something to paper over.
//
// Pure: no IO, no globals. The tool layer installs a value of this type
// (tool/web_search_policy.hpp); the runtime persists it through the settings
// registry like every other knob.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::web_search_cfg {

// Bounds the registry clamps to, and the tool layer re-clamps to. Named here
// so the table row and the clamp can never disagree.
//
// What a search can actually RETURN is lower than kMaxCountMax: mcp-cpp reads
// one result page from whichever engine answers, and Brave (tried first)
// serves at most 20 per page (observed live). So a ceiling or a request above
// 20 is accepted and clamped correctly, but yields no more than one page.
// The ceiling is a limit, not a promise; widening what a search returns
// needs pagination in mcp-cpp.
inline constexpr int kCountMin       = 1;
inline constexpr int kCountMax       = 20;
inline constexpr int kCountDefault   = 10;   // == mcp-cpp's own default
inline constexpr int kMaxCountMin    = 1;
inline constexpr int kMaxCountMax    = 50;
inline constexpr int kMaxCountDefault = 20;

// The web_search switch. `Auto` is how agentty behaved before this pane
// existed: the tool is advertised and the MODEL decides how many results to
// ask for (mcp-cpp's own default of 10 when it does not say). Nothing on the
// host — turn count, Smart Mode effort — feeds that number.
// `On` additionally applies the count the user set, as a default and as a
// ceiling. `Off` takes the tool off the wire and refuses calls to it.
// Exclusions apply in Auto and On alike.
enum class Mode : std::uint8_t { Auto, On, Off };

struct Config {
    // Stored as the row's text ("auto" | "on" | "off"), so the registry's
    // Enum type can hold it; mode() is the typed view every reader uses.
    std::string mode_text = "auto";

    // Results when the model does not ask for a number. Only applied in On.
    int count = kCountDefault;

    // Ceiling on what the model may ask for. Only applied in On. Also catches
    // the degenerate requests (0, negative) that would return no results.
    int max_count = kMaxCountDefault;

    // Domains never to return, space- or comma-separated as typed
    // ("pinterest.com, w3schools.com"). Kept as the user's text, not a parsed
    // list, so the pane shows exactly what was typed and settings.json stays
    // hand-editable; exclude_list() is the parsed view.
    std::string exclude_sites;

    [[nodiscard]] bool operator==(const Config&) const = default;
};

// The typed view of Config::mode_text. Anything unrecognised reads as Auto —
// the registry only ever writes the three words, and a hand-edited typo must
// not switch the tool off or on by accident.
[[nodiscard]] Mode mode(const Config& c);

// The parsed, normalised exclusion list: lower-cased, scheme / "www." /
// path / port stripped, empties and duplicates dropped, anything that is not
// a plausible host refused. Order preserved, so the rewritten query is
// stable for a given setting (and therefore cache-friendly upstream).
[[nodiscard]] std::vector<std::string> exclude_list(std::string_view text);

// The effective result count for a request: the model's own `count` when it
// gave a usable integer, the default otherwise, always clamped to
// [1, max_count].
[[nodiscard]] int effective_count(const Config& c, const nlohmann::json& args);

// Append `-site:<domain>` for every excluded domain the query does not
// already exclude itself. An operator the model wrote is left alone, so a
// query is never given the same exclusion twice.
[[nodiscard]] std::string apply_exclusions(std::string query,
                                           const std::vector<std::string>& sites);

// The whole request rewrite the dispatch seam applies. In On: count resolved
// and clamped. In Auto: the model's count passes through unchanged, exactly
// as before the pane existed. Both: exclusions appended. Arguments it does
// not own pass through untouched (display_description, and anything a future
// mcp-cpp adds). Off is refused by the caller before this runs.
[[nodiscard]] nlohmann::json rewrite_args(const Config& c, nlohmann::json args);

} // namespace agentty::web_search_cfg
