// web_search_plan.cpp — see the header.

#include "agentty/tool/web_search_plan.hpp"

#include "agentty/tool/web_search_policy.hpp"
#include "agentty/tool/web_search_secret.hpp"

#include <algorithm>

namespace agentty::tools::web_search_plan {

mcp::tools::WebSearchPlan build_plan(const web_search_cfg::Config& c,
                                  const KeyLookup& key_of) {
    ::mcp::tools::WebSearchPlan plan;
    if (c.language != "auto" && c.language.size() == 2) plan.language = c.language;

    for (const std::string* id : {&c.primary, &c.secondary, &c.fallback}) {
        if (id->empty() || *id == "none") continue;
        const auto* svc = ::mcp::tools::find_web_search_service(*id);
        if (!svc) continue;
        const bool dup = std::any_of(plan.slots.begin(), plan.slots.end(),
            [&](const ::mcp::tools::WebSearchSlot& s) { return s.service == *id; });
        if (dup) continue;

        ::mcp::tools::WebSearchSlot slot;
        slot.service = *id;
        if (svc->needs_key && key_of) slot.api_key = key_of(*id);
        const std::string prefix = *id + ".";
        for (const auto& [k, v] : c.dials)
            if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0)
                slot.dials.emplace_back(k.substr(prefix.size()), v);
        plan.slots.push_back(std::move(slot));
    }
    // Nothing usable (all three unset or unknown): fall back to the key-less
    // default rather than leave web_search with no way to answer.
    if (plan.slots.empty()) {
        auto d = ::mcp::tools::default_web_search_plan();
        d.language = plan.language;
        return d;
    }
    return plan;
}

namespace {

struct LiveSource final : ::mcp::tools::WebSearchPlanSource {
    ::mcp::tools::WebSearchPlan plan() const override {
        return build_plan(web_search_policy::current(),
                          [](std::string_view s) { return web_search_secret::load(s); });
    }
};

} // namespace

std::shared_ptr<::mcp::tools::WebSearchPlanSource> make_plan_source() {
    return std::make_shared<LiveSource>();
}

} // namespace agentty::tools::web_search_plan
