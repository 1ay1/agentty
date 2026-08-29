#pragma once
// agentty::provider::credentials — THE central, provider-uniform credential
// layer. Every subsystem that needs a provider's auth (the stream seam, the
// switch funnel, the account manager UI) goes through here, so there is ONE
// resolver and ONE account model for every provider — Anthropic (OAuth or
// key), the OpenAI-family hosted keys, ChatGPT / Copilot / Kimi (OAuth), a
// user-added custom host, and local servers (Ollama / llama.cpp, no auth).
//
// Design (mirrors Anthropic's account-manager flow, made uniform):
//   • A provider's ACTIVE credential is resolved by resolve(provider_id) —
//     stored account → env-var chain → empty. No per-provider special-casing
//     leaks into callers (the old resolve_auth_for's `anthropic_creds`
//     side-channel is gone; Anthropic is just "the provider whose store is
//     credentials.json").
//   • Where the secret physically LIVES differs per provider (a credentials
//     file, Settings.provider_keys[spec], or nothing for local) — that is the
//     ONLY thing that varies, captured behind backend_for() internally.
//   • Multi-account is uniform: list / active_label / activate / remove all
//     key on the provider id, so the same account-manager UI drives every
//     provider.
//
// This header declares the seam; the implementation lives in
// src/provider/credentials.cpp and delegates storage to auth::* (Anthropic),
// the provider oauth modules (chatgpt/copilot/kimi), and Settings.provider_keys
// (hosted keys + custom hosts) via the accounts registry.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/accounts.hpp"

namespace agentty::provider::credentials {

// How a provider takes a NEW account — drives the account-manager "add" flow.
enum class AddMethod : std::uint8_t {
    ApiKey,       // paste a bearer/API key (hosted OpenAI-family, custom host).
    OAuthDevice,  // browser/device OAuth (Anthropic, ChatGPT, Copilot, Kimi).
    None,         // local server (Ollama / llama.cpp) — no account.
};

// Resolve the auth header for `provider_id`'s ACTIVE credential. The single
// resolver, uniform across every provider:
//   stored active account (file OR provider_keys) → env-var chain → empty.
// Empty (for a provider that needs auth) means "not signed in" — callers route
// to the account manager. `AuthStyle::None` providers always resolve empty.
[[nodiscard]] auth::AuthHeader resolve(std::string_view provider_id);

// True when `provider_id` needs a credential the user hasn't provided yet
// (resolve() is empty AND the provider isn't a keyless local server). Drives
// "route to login on switch".
[[nodiscard]] bool needs_login(std::string_view provider_id);

// The add-account method this provider offers.
[[nodiscard]] AddMethod add_method(std::string_view provider_id);

// ── Account management — uniform for every provider ──────────────────────────
// Saved accounts for a provider (may be empty). Mirrors auth::accounts but
// keyed by the canonical provider id and consistent for file/key/local backends.
[[nodiscard]] std::vector<auth::accounts::Account> list(std::string_view provider_id);

// The active account's label ("OAuth · me@x", "key …Ddmj", "local", …).
[[nodiscard]] std::string active_label(std::string_view provider_id);

// Switch the active account for a provider to `label` (writes its secret into
// the provider's live store + updates the registry). Returns false if unknown.
bool activate(std::string_view provider_id, std::string_view label);

// Remove one saved account. When it was the active one, promotes the next (or
// clears the live credential if none remain).
bool remove(std::string_view provider_id, std::string_view label);

// Wipe the provider's LIVE active credential entirely (its file, or its
// provider_keys[spec] entry) — a full sign-out. Used when the last account is
// removed so build_account_list can't rediscover and resurrect it.
void clear_active(std::string_view provider_id);

// Persist a freshly-obtained API key as this provider's active account
// (snapshots any prior active account first, so it ADDS, not replaces).
bool add_key(std::string_view provider_id, std::string_view key);

} // namespace agentty::provider::credentials
