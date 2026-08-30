// provider::auth_state — see auth_state.hpp.

#include "agentty/provider/auth_state.hpp"

#include <cstdlib>

#include "agentty/provider/registry.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"

namespace agentty::provider {

namespace {

bool env_has(std::string_view name) {
    if (name.empty()) return false;
    const char* v = std::getenv(std::string{name}.c_str());
    return v != nullptr && *v != '\0';
}

} // namespace

bool provider_is_authed(const ProviderDescriptor& p,
                        const store::Settings& settings) {
    // Local / no-auth backends are always usable.
    if (p.is_local || p.auth == AuthStyle::None) return true;

    // In-process OAuth providers: ask their own stat-cached predicate.
    if (p.id == "chatgpt") return chatgpt::responses_available();
    if (p.id == "copilot") return copilot::signed_in();
    if (p.id == "kimi")    return kimi::signed_in();

    // Anthropic: OAuth (Pro/Max) or x-api-key on disk — independent of the
    // currently-active provider.
    if (kind_of(p.wire) == Kind::Anthropic)
        return auth::anthropic_signed_in()
            || settings.provider_keys.count(std::string{p.id}) != 0;

    // Hosted OpenAI-family: a bearer key from env OR the saved provider_keys.
    for (auto env : p.auth_env)
        if (env_has(env)) return true;
    if (auto it = settings.provider_keys.find(std::string{p.id});
        it != settings.provider_keys.end() && !it->second.empty())
        return true;
    return false;
}

bool provider_is_authed(std::string_view id, const store::Settings& settings) {
    if (const ProviderDescriptor* p = preset_for(id))
        return provider_is_authed(*p, settings);
    // Not a registry preset ⇒ a saved custom host. Authed iff it has a
    // provider_keys entry at all (a keyless local host is saved with an empty
    // value and is still usable).
    return settings.provider_keys.count(std::string{id}) != 0;
}

AuthSource auth_source(const ProviderDescriptor& p,
                       const store::Settings& settings) {
    if (p.is_local || p.auth == AuthStyle::None) return AuthSource::Local;
    // In-process OAuth + Anthropic: a saved credential (disk/token) or a
    // pasted key. These are all in-app removable, so "Saved".
    if (p.id == "chatgpt" || p.id == "copilot" || p.id == "kimi"
        || kind_of(p.wire) == Kind::Anthropic)
        return provider_is_authed(p, settings) ? AuthSource::Saved
                                               : AuthSource::None;
    // Hosted OpenAI-family: a pasted key (removable) takes precedence in
    // resolution, so report Saved when one exists; otherwise Env iff any of
    // the provider's env vars is set (that key can't be dropped in-app).
    if (auto it = settings.provider_keys.find(std::string{p.id});
        it != settings.provider_keys.end() && !it->second.empty())
        return AuthSource::Saved;
    for (auto env : p.auth_env)
        if (env_has(env)) return AuthSource::Env;
    return AuthSource::None;
}

} // namespace agentty::provider
