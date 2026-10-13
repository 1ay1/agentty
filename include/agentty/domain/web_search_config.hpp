#pragma once
// agentty::web_search_cfg — the user's policy for the `web_search` tool.
//
// ── What the host decides ────────────────────────────────────────────────
//
// web_search is implemented in mcp-cpp (third_party/mcp-cpp/src/tools/web.cpp):
// the catalogue of services, each one's request and reply format, the HTML
// parsers and the dedup all live in that library, a separate repository.
// agentty owns the CHOICES and hands them over at three seams:
//
//   * advertisement — tools::select_wire_tools decides what goes on the wire
//   * dispatch      — the bridge's execute closure rewrites the call's args
//                     (count, exclusions)
//   * the plan      — tools::web_search_plan turns the slots, language and per-
//                     service dials below into the WebSearchPlan mcp-cpp follows,
//                     read afresh on every search
//
// API keys are not part of this type. They are secrets, so they live in
// tool/web_search_secret.hpp (keystore, else a sealed file) and are joined to the
// plan only when it is built; nothing here can carry one into settings.json.
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
// `NOT site:`). Services with a native domain filter (Tavily, Firecrawl, Exa)
// receive the list as a request field instead, which mcp-cpp does by lifting
// the `-site:` tokens back out of the query.
//
// Pure: no IO, no globals. The tool layer installs a value of this type
// (tool/web_search_policy.hpp); the runtime persists it through the settings
// registry like every other knob.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::web_search_cfg {

// Bounds the registry clamps to, and the tool layer re-clamps to. Named here
// so the table row and the clamp can never disagree.
//
// What a search can actually RETURN is capped per service: mcp-cpp asks the
// service that answers for one result page and clamps the count to that
// service's documented maximum (WebSearchService::max_count: 20 for Brave, Tavily,
// Serper and Perplexity, 30 for DuckDuckGo, 100 for Firecrawl and Exa). So a
// ceiling above a service's cap is accepted and clamped correctly, but yields
// no more than that service gives. The ceiling is a limit, not a promise.
inline constexpr int kCountMin       = 1;
inline constexpr int kCountMax       = 20;
inline constexpr int kCountDefault   = 10;   // == mcp-cpp's own default
inline constexpr int kMaxCountMin    = 1;
inline constexpr int kMaxCountMax    = 50;
inline constexpr int kMaxCountDefault = 20;

// The services a slot can name: the ids of mcp-cpp's catalogue, in menu order
// (Free first, then API key). Spelled out here because the registry needs the
// list at compile time and the catalogue is a runtime value; a test fails if
// the two ever disagree. Primary cannot be empty; the other two can.
inline constexpr std::string_view kServiceIds =
    "brave-free|ddg-free|tavily-free|firecrawl-free|"
    "brave-api|tavily-api|firecrawl-api|exa-api|serper-api|perplexity-api";
inline constexpr std::string_view kOptionalServiceIds =
    "none|brave-free|ddg-free|tavily-free|firecrawl-free|"
    "brave-api|tavily-api|firecrawl-api|exa-api|serper-api|perplexity-api";
// ISO 639-1, the languages the keyed services document support for.
inline constexpr std::string_view kLanguages =
    "auto|en|de|fr|es|it|pt|nl|sv|da|fi|pl|cs|ru|uk|tr|ja|ko|zh|ar|he|hi";

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

    // Which services web_search tries, in order: the first to answer wins.
    // Stored as the catalogue's service id ("brave-free", "exa-api"); the
    // catalogue itself lives in mcp-cpp (mcp/tools/web_search.hpp), so this
    // domain type holds only the ids. "none" leaves a slot empty (Primary
    // cannot be: a search with no service cannot run). The defaults need no
    // account, card or key.
    std::string primary   = "brave-free";
    std::string secondary = "ddg-free";
    std::string fallback  = "tavily-free";

    // ISO 639-1 code sent to the services that take one, or "auto" for each
    // service's own default. Services that cannot take a language ignore it.
    std::string language = "auto";

    // Per-SERVICE extra settings (freshness, region, ...), keyed
    // "<service-id>.<dial-id>" -> value id. Per service rather than per slot
    // so choosing Exa for Fallback after trying it as Primary keeps what was
    // set. Only non-default values are stored. Values are not checked here:
    // mcp-cpp sends a value only if the service lists it.
    std::map<std::string, std::string> dials;

    [[nodiscard]] bool operator==(const Config&) const = default;
};

// What a stored dial entry must look like: key "<service>.<dial>" and a value,
// both short and printable. Applied on load AND on save, so a hand-edited
// settings.json cannot hold what the pane could not produce, and junk keys
// cannot crowd real ones out of the cap.
inline constexpr std::size_t kMaxDials = 64;
[[nodiscard]] bool dial_entry_ok(std::string_view key, std::string_view value) noexcept;

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
