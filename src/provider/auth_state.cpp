// provider::auth_state — see auth_state.hpp.

#include "agentty/provider/auth_state.hpp"

#include <cstdlib>

#include "agentty/provider/registry.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/auth/vault.hpp"
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

    // Dispatch on the vault DESCRIPTOR (which store family this provider
    // uses) but answer the SettingsKey question from the INJECTED settings —
    // this function is a pure query over its inputs (tests inject an
    // in-memory Settings; the vault's own accessor reads disk and would
    // break that hermeticity and the DI discipline).
    switch (auth::vault::of(p.id).kind) {
        case auth::vault::Kind::AnthropicFile:
            if (auth::anthropic_signed_in()) return true;
            // Pasted-key overlap: provider_keys["anthropic"].
            return settings.provider_keys.count(std::string{p.id}) != 0;
        case auth::vault::Kind::OAuthFile:
            return auth::vault::signed_in(std::string{p.id});
        case auth::vault::Kind::SettingsKey:
        case auth::vault::Kind::None:
            break;
    }
    // Hosted key: saved entry in the injected settings, or an env var.
    if (auto it = settings.provider_keys.find(std::string{p.id});
        it != settings.provider_keys.end() && !it->second.empty())
        return true;
    for (auto env : p.auth_env)
        if (env_has(env)) return true;
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
    // Same dispatch discipline as provider_is_authed: vault kind selects
    // the store family; SettingsKey answers come from the INJECTED settings.
    switch (auth::vault::of(p.id).kind) {
        case auth::vault::Kind::AnthropicFile:
            if (auth::anthropic_signed_in()
                || settings.provider_keys.count(std::string{p.id}) != 0)
                return AuthSource::Saved;
            return AuthSource::None;
        case auth::vault::Kind::OAuthFile:
            return auth::vault::signed_in(std::string{p.id})
                 ? AuthSource::Saved : AuthSource::None;
        case auth::vault::Kind::SettingsKey:
        case auth::vault::Kind::None:
            break;
    }
    if (auto it = settings.provider_keys.find(std::string{p.id});
        it != settings.provider_keys.end() && !it->second.empty())
        return AuthSource::Saved;
    // Env-only credential: usable but NOT removable in-app.
    for (auto env : p.auth_env)
        if (env_has(env)) return AuthSource::Env;
    return AuthSource::None;
}

} // namespace agentty::provider
