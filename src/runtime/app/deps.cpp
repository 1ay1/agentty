#include "agentty/runtime/app/deps.hpp"

#include "agentty/tool/subagent.hpp"

#include <stdexcept>

namespace agentty::app {

namespace {
Deps* g_deps = nullptr;
}

const Deps& deps() {
    if (!g_deps) throw std::logic_error("agentty::app::deps() called before install_deps()");
    return *g_deps;
}

void install_deps(Deps d) {
    static Deps storage;
    storage = std::move(d);
    g_deps = &storage;
}

void update_auth(auth::AuthHeader auth) {
    if (!g_deps) return;
    g_deps->auth = std::move(auth);
    tools::subagent::set_auth(g_deps->auth);
}

void switch_provider(auth::AuthHeader auth) {
    // The active provider::Selection is process-global and is set by the
    // reducer via provider::select() before this runs; the stream seam
    // dispatches on provider::active() at call time. All this seam does is
    // re-point Deps::auth at the new backend's credentials so the next
    // request authenticates correctly.
    if (!g_deps) return;
    g_deps->auth = std::move(auth);
    tools::subagent::set_auth(g_deps->auth);
}

} // namespace agentty::app
