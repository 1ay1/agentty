// agentty::auth::vault — the ONE table answering every per-provider auth
// question. See vault.hpp for the design rationale.

#include "agentty/auth/vault.hpp"

#include "agentty/auth/accounts.hpp"          // derive_current_label
#include "agentty/io/persistence.hpp"         // load/save_settings
#include "agentty/provider/credentials.hpp"   // central resolve()
#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"

namespace agentty::auth::vault {

namespace {

// ── Shared behaviours ────────────────────────────────────────────────────
// The central credential resolver already dispatches correctly per provider
// — every descriptor delegates to it, so the WIRE story stays single-source
// (vault adds the storage/lifecycle story around it, it does not fork auth).
auth::AuthHeader resolve_central(const std::string& p) {
    return provider::credentials::resolve(p);
}

std::string label_from_registry(const std::string& p) {
    return auth::accounts::derive_current_label(p);
}

// SettingsKey family (hosted API keys AND custom hosts): the credential is
// Settings.provider_keys[p]; signed-in iff a non-empty entry exists (a
// custom LOCAL host saves an empty value and is usable keylessly, but that
// is Kind::None territory — provider_is_authed handles the nuance for
// pickers; the vault's question is "is a secret SAVED here").
bool key_signed_in(const std::string& p) {
    auto s = persistence::load_settings();
    auto it = s.provider_keys.find(p);
    return it != s.provider_keys.end() && !it->second.empty();
}
void key_clear(const std::string& p) {
    auto s = persistence::load_settings();
    if (s.provider_keys.erase(p) > 0) persistence::save_settings(s);
}

void noop_after(const std::string&) {}

// ── Per-kind rows ────────────────────────────────────────────────────────
constexpr Desc kAnthropic{
    .id           = "anthropic",
    .kind         = Kind::AnthropicFile,
    .is_signed_in = [](const std::string&) { return auth::anthropic_signed_in(); },
    .clear        = [](const std::string&) { (void)auth::clear_credentials(); },
    .resolve      = resolve_central,
    .current_label = label_from_registry,
    .after_activate = [](const std::string&) {
        // Re-seal into the keystore so the next resolve() reads the
        // switched-to account, not a stale cache.
        if (auto c = auth::load_credentials()) auth::save_credentials(*c);
    },
};

constexpr Desc kChatGpt{
    .id           = "chatgpt",
    .kind         = Kind::OAuthFile,
    .is_signed_in = [](const std::string&) {
        return provider::chatgpt::responses_available();
    },
    .clear = [](const std::string&) {
        (void)provider::chatgpt::clear_codex_credentials();
    },
    .resolve      = resolve_central,   // empty: the transport owns the token
    .current_label = label_from_registry,
    .after_activate = [](const std::string&) {
        if (auto c = provider::chatgpt::load_codex_credentials())
            provider::chatgpt::save_codex_credentials(*c);
    },
};

constexpr Desc kCopilot{
    .id           = "copilot",
    .kind         = Kind::OAuthFile,
    .is_signed_in = [](const std::string&) {
        return provider::copilot::signed_in();
    },
    .clear = [](const std::string&) {
        (void)provider::copilot::clear_credentials();
    },
    .resolve      = resolve_central,
    .current_label = label_from_registry,
    .after_activate = [](const std::string&) {
        provider::copilot::invalidate_cached_token();
    },
};

constexpr Desc kKimi{
    .id           = "kimi",
    .kind         = Kind::OAuthFile,
    .is_signed_in = [](const std::string&) {
        return provider::kimi::signed_in();
    },
    .clear = [](const std::string&) {
        (void)provider::kimi::clear_credentials();
    },
    .resolve      = resolve_central,
    .current_label = label_from_registry,
    .after_activate = [](const std::string&) {
        provider::kimi::invalidate_cached_token();
    },
};

// EVERY hosted-key preset (groq/mistral/openrouter/…) and every custom host:
// one shared row — the provider id passed through selects the key slot.
constexpr Desc kSettingsKey{
    .id           = "",                 // synthesized; id is the argument
    .kind         = Kind::SettingsKey,
    .is_signed_in = key_signed_in,
    .clear        = key_clear,
    .resolve      = resolve_central,
    .current_label = label_from_registry,
    .after_activate = noop_after,
};

} // namespace

const Desc& of(std::string_view provider_id) {
    if (provider_id.empty() || provider_id == "anthropic") return kAnthropic;
    if (provider_id == "chatgpt" || provider_id == "codex-cli") return kChatGpt;
    if (provider_id == "copilot") return kCopilot;
    if (provider_id == "kimi")    return kKimi;
    return kSettingsKey;   // hosted keys + custom hosts, uniformly
}

} // namespace agentty::auth::vault
