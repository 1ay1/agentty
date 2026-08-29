// agentty::provider::credentials — implementation. See credentials.hpp for the
// design. Storage backends per provider:
//   • Anthropic          → auth::credentials.json (OAuth or x-api-key).
//   • ChatGPT/Copilot/Kimi (oauth_native) → their own credential files, read at
//     REQUEST time by their transports; resolve() returns an empty AuthHeader
//     (the transport supplies the token). We still expose account mgmt via the
//     accounts registry, which snapshots those files.
//   • Hosted API-key presets + custom hosts → Settings.provider_keys[id].
//   • Local (AuthStyle::None) → no credential.

#include "agentty/provider/credentials.hpp"

#include <cstdlib>

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/io/persistence.hpp"

namespace agentty::provider::credentials {

namespace {

std::string env_or_empty(std::string_view name) {
    if (name.empty()) return {};
    const char* v = std::getenv(std::string{name}.c_str());
    return v ? std::string{v} : std::string{};
}

// The provider descriptor for an id (nullptr ⇒ a custom host / unknown spec).
const ProviderPreset* preset(std::string_view id) { return preset_for(id); }

bool is_anthropic(std::string_view id) {
    const auto* p = preset(id);
    return p ? p->kind() == Kind::Anthropic
             : parse_selection(id).kind == Kind::Anthropic;
}

// oauth_native providers manage their own token at request time.
bool is_oauth_native(std::string_view id) {
    const auto* p = preset(id);
    return p && p->oauth_native;
}

bool is_local(std::string_view id) {
    const auto* p = preset(id);
    return p && (p->is_local || p->auth == AuthStyle::None);
}

} // namespace

auth::AuthHeader resolve(std::string_view provider_id) {
    // Anthropic: OAuth or x-api-key from its credential file.
    if (is_anthropic(provider_id)) {
        if (auto c = auth::load_credentials())
            return auth::make_auth_header(*c);
        return auth::AuthHeader{};   // not signed in
    }
    // oauth_native (ChatGPT/Copilot/Kimi) and local servers: the transport
    // reads the live token / needs none — no AuthHeader from here.
    if (is_oauth_native(provider_id) || is_local(provider_id))
        return auth::AuthHeader{auth::ApiKeyHeader{std::string{}}};

    // Hosted OpenAI-family key OR custom host: bearer key, precedence
    //   saved provider_keys[id]  >  env-var chain  >  OPENAI_API_KEY.
    std::string key;
    {
        auto s = persistence::load_settings();
        if (auto it = s.provider_keys.find(std::string{provider_id});
            it != s.provider_keys.end())
            key = it->second;
    }
    if (key.empty()) {
        if (const auto* p = preset(provider_id))
            for (auto env : p->auth_env)
                if (!(key = env_or_empty(env)).empty()) break;
    }
    if (key.empty()) key = env_or_empty("OPENAI_API_KEY");
    return auth::AuthHeader{auth::ApiKeyHeader{std::move(key)}};
}

bool needs_login(std::string_view provider_id) {
    if (is_local(provider_id)) return false;         // keyless
    if (is_oauth_native(provider_id)) {
        // Signed in iff the accounts registry has an active account for it
        // (the transport reads the file; empty registry ⇒ not signed in).
        return auth::accounts::list_for(std::string{provider_id}).empty()
            && auth::accounts::active_label(std::string{provider_id}).empty();
    }
    return auth::bearer_token(resolve(provider_id)).empty();
}

AddMethod add_method(std::string_view provider_id) {
    if (is_local(provider_id)) return AddMethod::None;
    if (is_anthropic(provider_id) || is_oauth_native(provider_id))
        return AddMethod::OAuthDevice;
    return AddMethod::ApiKey;   // hosted key + custom host
}

std::vector<auth::accounts::Account> list(std::string_view provider_id) {
    return auth::accounts::list_for(std::string{provider_id});
}

std::string active_label(std::string_view provider_id) {
    auto lbl = auth::accounts::active_label(std::string{provider_id});
    if (!lbl.empty()) return lbl;
    // No registered account yet — derive a live label from the active cred.
    return auth::accounts::derive_current_label(std::string{provider_id});
}

bool activate(std::string_view provider_id, std::string_view label) {
    return auth::accounts::activate(std::string{provider_id}, std::string{label});
}

bool remove(std::string_view provider_id, std::string_view label) {
    return auth::accounts::remove(std::string{provider_id}, std::string{label});
}

bool add_key(std::string_view provider_id, std::string_view key) {
    const std::string id{provider_id};
    auto s = persistence::load_settings();
    // Preserve any existing key as a switchable account BEFORE overwriting,
    // so "add another account" adds rather than replaces.
    if (auto it = s.provider_keys.find(id);
        it != s.provider_keys.end() && !it->second.empty()
        && it->second != std::string{key}) {
        if (auto prior = auth::accounts::derive_current_label(id); !prior.empty())
            auth::accounts::snapshot_active(id, prior);
    }
    s.provider_keys[id] = std::string{key};
    persistence::save_settings(s);
    if (auto lbl = auth::accounts::derive_current_label(id); !lbl.empty())
        auth::accounts::snapshot_active(id, lbl);
    return true;
}

} // namespace agentty::provider::credentials
