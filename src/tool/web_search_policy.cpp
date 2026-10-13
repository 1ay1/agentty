// web_search_policy.cpp — the process-wide web_search policy slot.

#include "agentty/tool/web_search_policy.hpp"

#include "agentty/io/persistence.hpp"
#include "agentty/util/dbglog.hpp"
#include "agentty/util/snapshot.hpp"

#include <exception>

namespace agentty::tools::web_search_policy {

namespace {

util::AtomicSnapshot<web_search_cfg::Config>& slot() {
    static util::AtomicSnapshot<web_search_cfg::Config> s;
    return s;
}

} // namespace

void seed(Io io) {
    // From settings.json, once, by an entry point that may read disk. Never
    // overwrites a value the pane already installed.
    if (slot().load()) return;
    web_search_cfg::Config c;
    try {
        c = persistence::load_settings(io).web_search;   // applies env overrides
    } catch (const std::exception& e) {
        util::dbglog("web_search_policy.seed", e.what());
    } catch (...) {
        util::dbglog("web_search_policy.seed", "non-std exception");
    }
    // Atomic: a pane install that lands while the disk read runs wins.
    (void)slot().store_if_empty(std::move(c));
}

web_search_cfg::Config current() {
    const auto s = slot().load();
    return s ? *s : web_search_cfg::Config{};
}

void install(web_search_cfg::Config c) {
    slot().store(std::move(c));
}

} // namespace agentty::tools::web_search_policy
