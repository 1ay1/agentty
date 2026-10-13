#pragma once
// AuthView — what the Model knows about credentials that live outside the
// settings record.
//
// A provider's sign-in state is spread over credentials.json, provider-owned
// OAuth token files, env vars and the accounts registry. Reducers and views
// used to ask the disk directly, which made update impure and re-statted
// those files every frame.
//
// The host reads them into this value (LoadAuthView effect, replying
// AuthViewLoaded) at launch and after every effect that changes credentials.
// Keys pasted into settings are NOT here: those are m.d.persisted.provider_keys.
// provider::auth_source(p, settings, view) combines the two.

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::auth {

struct ProviderAuth {
    // A credential in this provider's own store: credentials.json for
    // Anthropic, the token file for ChatGPT/Copilot/Kimi.
    bool                     stored = false;
    // The first env var from the provider's auth_env that is set, if any.
    std::string              env_var;
    // The accounts registry for this provider, newest first, and which one
    // is active.
    std::vector<std::string> accounts;
    std::string              active_account;
    // A live credential not yet in the registry (a legacy single login). The
    // account list registers it under this label when opened.
    std::string              unregistered_label;

    bool operator==(const ProviderAuth&) const = default;
};

struct AuthView {
    std::map<std::string, ProviderAuth, std::less<>> providers;
    // False until the host has loaded it once.
    bool loaded = false;

    [[nodiscard]] const ProviderAuth* find(std::string_view id) const {
        auto it = providers.find(id);
        return it == providers.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool stored(std::string_view id) const {
        const auto* p = find(id);
        return p && p->stored;
    }
    [[nodiscard]] std::string env_var(std::string_view id) const {
        const auto* p = find(id);
        return p ? p->env_var : std::string{};
    }
    [[nodiscard]] std::string active_account(std::string_view id) const {
        const auto* p = find(id);
        return p ? p->active_account : std::string{};
    }
    bool operator==(const AuthView&) const = default;
};

} // namespace agentty::auth
