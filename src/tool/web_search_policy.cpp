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

// Seed from disk exactly once. A function-local static is initialised under
// the language's own once-guard, so a reader racing install() either runs the
// seed first or waits for it — install() calls this too, which is what keeps
// a seed that finishes LATE from overwriting a value the pane just installed.
bool seeded() {
    static const bool once = [] {
        web_search_cfg::Config c;
        try {
            // load_settings() applies the env overrides itself.
            c = persistence::load_settings().web_search;
        } catch (const std::exception& e) {
            util::dbglog("web_search_policy.seed", e.what());
        } catch (...) {
            util::dbglog("web_search_policy.seed", "non-std exception");
        }
        slot().store(std::move(c));
        return true;
    }();
    return once;
}

} // namespace

web_search_cfg::Config current() {
    (void)seeded();
    const auto s = slot().load();
    return s ? *s : web_search_cfg::Config{};
}

void install(web_search_cfg::Config c) {
    (void)seeded();
    slot().store(std::move(c));
}

} // namespace agentty::tools::web_search_policy
