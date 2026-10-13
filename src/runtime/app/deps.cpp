#include "agentty/runtime/app/deps.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/registry.hpp"

#include <maya/runtime.hpp>
#include <stdexcept>
#include <variant>

namespace agentty::app {

namespace {
Deps* g_deps = nullptr;
// The live auth header. Written by the host's InstallAuth effect on the loop
// thread, read by workers (subagents, the catalog fetch, every stream). A
// snapshot, not a lock: it is read far more than written and from inside
// other locks' bodies (provider caches), so a lock here would need a rank
// above all of them. Lock-free, it ranks nowhere and can't deadlock.
maya::snapshot<auth::AuthHeader>& g_auth() {
    static maya::snapshot<auth::AuthHeader> a;
    return a;
}
}

auth::AuthHeader live_auth() { return g_auth().get(); }

const Deps& deps(Io) {
    if (!g_deps) throw std::logic_error("agentty::app::deps() called before install_deps()");
    return *g_deps;
}

void install_deps(Deps d) {
    static Deps storage;
    g_auth().store(d.auth);
    storage = std::move(d);
    g_deps = &storage;
}

auth::AuthHeader auth_snapshot(Io io, const provider::Selection& sel) {
    // Resolve from the given provider through the central credential layer,
    // so the credential can never drift from the provider it is for. This is
    // the single source of truth for "what auth goes on the wire": if a switch
    // changed the active provider/model but a code path forgot to reinstall
    // the header, this still returns the RIGHT provider's credential (the
    // class of bug behind Anthropic's OAuth token being sent to Mistral → 401).
    //
    // Anthropic and hosted-key/custom-host providers resolve a real header
    // here; oauth_native (ChatGPT/Copilot/Kimi) and local resolve empty and
    // their transports supply the token — identical to update_auth's cache,
    // which we keep as a fast/fallback path for those.
    const std::string pid =
        sel.kind == provider::Kind::OpenAI ? sel.openai_endpoint.label
                                           : std::string{provider::default_provider_id()};
    auto resolved = provider::credentials::resolve(io, pid);
    if (!auth::bearer_token(resolved).empty()
        || std::holds_alternative<auth::BearerHeader>(resolved))
        return resolved;
    // Empty resolve (oauth_native / local) — fall back to the cached header the
    // login/switch flow installed (used by those providers' transports).
    return live_auth();
}

// Off the loop (a worker with no Model): resolve against the PUBLISHED copy of
// the selection, which the dispatch seam keeps equal to the Model's.
auth::AuthHeader auth_snapshot(Io io) {
    return auth_snapshot(io, provider::active());
}

void update_auth(Io io, auth::AuthHeader auth) {
    g_auth().store(auth);
    tools::subagent::set_auth(io, std::move(auth));
}

} // namespace agentty::app
