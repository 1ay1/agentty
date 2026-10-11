#pragma once
// agentty::tools::web_search_plan — turns the user's search settings into the plan
// mcp-cpp's web_search follows.
//
// Two pieces, split so the interesting one needs no IO:
//   build_plan()      pure: settings + a key lookup -> WebSearchPlan
//   make_plan_source() the WebSearchPlanSource the bridge hands mcp-cpp. It reads
//                     the live policy and the key store on EVERY search, so a
//                     change in Settings reaches the next call, no restart.

#include "agentty/domain/web_search_config.hpp"

#include <mcp/tools/web_search.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace agentty::tools::web_search_plan {

using KeyLookup = std::function<std::string(std::string_view service)>;

// Primary, Secondary, Fallback in that order. A slot set to "none", to an id
// the catalogue does not know (a hand-edited file, a service a newer build
// added), or to a service an earlier slot already names is left out: asking
// the same service twice in one search only spends its rate limit.
//
// Each slot carries the dials stored for ITS service, and the key for it when
// the service wants one. A keyed service with no key is kept in the plan on
// purpose, so the search report can say "no API key set" instead of silently
// skipping a slot the user chose.
[[nodiscard]] ::mcp::tools::WebSearchPlan build_plan(const web_search_cfg::Config& c,
                                                const KeyLookup& key_of);

// Live source: web_search_policy::current() + web_search_secret::load().
[[nodiscard]] std::shared_ptr<::mcp::tools::WebSearchPlanSource> make_plan_source();

} // namespace agentty::tools::web_search_plan
