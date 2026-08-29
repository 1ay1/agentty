#include "agentty/runtime/app/deps.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/registry.hpp"

#include <mutex>
#include <stdexcept>
#include <variant>

namespace agentty::app {

namespace {
Deps* g_deps = nullptr;
// Guards Deps::auth across the UI/worker thread split (same reason
// provider::g_active has its lock): update_auth / switch_provider
// move-assign on the UI thread while worker tasks (fetch_models) read.
std::mutex g_auth_mu;
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

auth::AuthHeader auth_snapshot() {
    // Resolve from the CURRENTLY-ACTIVE provider through the central credential
    // layer, so the credential can never drift from provider::active(). This is
    // the single source of truth for "what auth goes on the wire": if a switch
    // changed the active provider/model but a code path forgot to reinstall the
    // header, this still returns the RIGHT provider's credential (the class of
    // bug behind Anthropic's OAuth token being sent to Mistral → 401).
    //
    // Anthropic and hosted-key/custom-host providers resolve a real header
    // here; oauth_native (ChatGPT/Copilot/Kimi) and local resolve empty and
    // their transports supply the token — identical to update_auth's cache,
    // which we keep as a fast/fallback path for those.
    const auto sel = provider::active();
    const std::string pid =
        sel.kind == provider::Kind::OpenAI ? sel.openai_endpoint.label
                                           : std::string{provider::default_provider_id()};
    auto resolved = provider::credentials::resolve(pid);
    if (!auth::bearer_token(resolved).empty()
        || std::holds_alternative<auth::BearerHeader>(resolved))
        return resolved;
    // Empty resolve (oauth_native / local) — fall back to the cached header the
    // login/switch flow installed (used by those providers' transports).
    std::lock_guard lk(g_auth_mu);
    return g_deps ? g_deps->auth : auth::AuthHeader{};
}

void update_auth(auth::AuthHeader auth) {
    if (!g_deps) return;
    {
        std::lock_guard lk(g_auth_mu);
        g_deps->auth = std::move(auth);
    }
    tools::subagent::set_auth(g_deps->auth);
}

void switch_provider(auth::AuthHeader auth) {
    // The active provider::Selection is process-global and is set by the
    // reducer via provider::select() before this runs; the stream seam
    // dispatches on provider::active() at call time. All this seam does is
    // re-point Deps::auth at the new backend's credentials so the next
    // request authenticates correctly.
    if (!g_deps) return;
    {
        std::lock_guard lk(g_auth_mu);
        g_deps->auth = std::move(auth);
    }
    tools::subagent::set_auth(g_deps->auth);
}

} // namespace agentty::app
