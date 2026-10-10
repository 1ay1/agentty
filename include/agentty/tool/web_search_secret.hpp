#pragma once
// agentty::tools::web_search_secret — where a web_search service's API key lives.
//
// Not in settings.json, for the reason embed_secret.hpp gives: a credential in
// a plaintext config file is a leak waiting for a screen-share or a dotfiles
// commit. Same two tiers as the rest of agentty's credential handling:
//   1. the OS keystore, when available and enabled (AGENTTY_USE_KEYSTORE);
//   2. otherwise a sealed file (auth::crypt::seal, machine-bound AEAD).
//
// Why not the provider-key vault (auth::keys): persistence rewrites that vault
// wholesale from Model.provider_keys on every settings save, which would wipe
// anything stored beside the provider keys, and every non-preset entry in it
// is shown as a custom provider host. These keys are not providers.
//
// Keyed by SERVICE id ("exa-api"), the catalogue id from mcp-cpp. The free
// services have nothing to store and are never passed here.
//
// An environment variable beats the store, so a key can come from CI or a
// secrets manager without ever touching disk:
//     AGENTTY_WEB_SEARCH_KEY_<ID>   with '-' turned into '_', upper case
//     e.g. AGENTTY_WEB_SEARCH_KEY_EXA_API, AGENTTY_WEB_SEARCH_KEY_BRAVE_API

#include <string>
#include <string_view>

namespace agentty::tools::web_search_secret {

// The variable that would override `service`'s key.
[[nodiscard]] std::string env_name(std::string_view service);

// The key to use ("" when none). Environment first, then keystore, then the
// sealed file. Never throws.
[[nodiscard]] std::string load(std::string_view service);

// True when load() would return something.
[[nodiscard]] bool has(std::string_view service);

// "env: AGENTTY_WEB_SEARCH_KEY_X", "stored", or "" — for the pane's provenance
// column. Never returns the key.
[[nodiscard]] std::string origin(std::string_view service);

// A key we are willing to put in a request header: 8..512 bytes, no spaces and
// no control characters. Checked before storing so a pasted line break or a
// "Bearer " prefix is caught at the pane, not as an opaque 401 later.
[[nodiscard]] bool plausible(std::string_view key);

// Persist. An empty key erases. Returns false if the secret could not be
// secured — callers must treat that as "not saved", never write plaintext.
bool store(std::string_view service, std::string_view key);

bool erase(std::string_view service);

} // namespace agentty::tools::web_search_secret
