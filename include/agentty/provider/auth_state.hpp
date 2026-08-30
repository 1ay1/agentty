#pragma once
// provider::auth_state — "can I switch to this provider RIGHT NOW without
// signing in?" as ONE reusable predicate over the registry, generalizing the
// per-provider auth badge the provider picker hand-rolled inline.
//
// The fused cross-provider model picker needs to split every provider into
// "authed → show its models" vs "un-authed → show a single sign-in offer".
// Each provider authenticates differently (Anthropic OAuth-or-key on disk,
// ChatGPT/Copilot/Kimi in-process OAuth predicates, hosted OpenAI-family a
// bearer key from env or Settings.provider_keys, local backends none), so a
// single dispatch point keeps the picker, the badge, and the login gate from
// drifting — the registry-as-SSOT discipline.

#include <string>
#include <string_view>

#include "agentty/store/store.hpp"   // store::Settings

namespace agentty::provider {

struct ProviderDescriptor;   // registry.hpp

// True when a turn could be streamed against `p` immediately: OAuth creds on
// disk / in-process, or an API key resolvable from env or the saved
// provider_keys, or a local (no-auth) backend. `settings` supplies the saved
// custom-host / API-key map (provider_keys). Network-free and cheap (the
// OAuth predicates are stat-cached).
[[nodiscard]] bool provider_is_authed(const ProviderDescriptor& p,
                                      const store::Settings& settings);

// Convenience overload keyed by provider id (looks the descriptor up in the
// registry). Unknown id ⇒ treated as a saved custom host: authed iff it has a
// provider_keys entry (a keyless local host counts as authed).
[[nodiscard]] bool provider_is_authed(std::string_view id,
                                      const store::Settings& settings);

// WHERE a provider's credential comes from — lets the picker tell the user
// "signed in" from "key from env" (the latter can't be signed out in-app,
// so ^D there is a no-op and would otherwise look broken).
enum class AuthSource {
    None,        // not authed
    Saved,       // OAuth on disk / in-process, or a pasted provider_keys entry
    Env,         // resolved ONLY from an environment variable
    Local,       // no-auth backend (Ollama, custom http host)
};
[[nodiscard]] AuthSource auth_source(const ProviderDescriptor& p,
                                     const store::Settings& settings);

} // namespace agentty::provider
