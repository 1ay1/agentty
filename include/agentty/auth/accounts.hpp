#pragma once
// agentty::auth::accounts — named, per-provider credential slots so a user
// can hold MULTIPLE accounts on the same provider and switch between them
// entirely in-app (never shelling out to re-login just to change who they
// are).
//
// Design: agentty already has one "active" credential store per provider
//   - Anthropic  : credentials.json (auth::save/load/clear_credentials)
//   - ChatGPT    : the Codex encrypted store (provider::chatgpt::*_codex_*)
//   - OpenAI-fam : Settings.provider_keys[endpoint-label]
//
// This module layers a REGISTRY (accounts.json, encrypted at rest with the
// same crypt::seal machinery as credentials.json) that records named
// SNAPSHOTS of a provider's credential. It never replaces the active-store
// paths — switching an account just copies a snapshot back into the active
// store and re-installs the live auth header. That keeps every existing
// resolve()/refresh() codepath working unchanged and is fully backward
// compatible: a user with only the legacy single credential sees exactly
// one account ("default") synthesised from it on first read.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agentty::auth::accounts {

// One saved account. `secret` is the provider-specific credential blob,
// serialized exactly as that provider's active store would write it:
//   - "anthropic" → the credentials.json body (method/access_token/…)
//   - "chatgpt"   → the Codex credential JSON
//   - openai ids  → the raw API key string
// The registry treats it opaquely; only the provider-specific adapter in
// account_switch.cpp knows how to install it.
struct Account {
    std::string  provider;      // canonical provider id ("anthropic", "chatgpt", "openai", …)
    std::string  label;         // user-facing name ("work", "personal", an email, …)
    std::string  secret;        // opaque credential blob (see above)
    std::int64_t saved_at_ms = 0;
};

// Every account across all providers, newest-saved first. Reads accounts.json;
// returns {} when none saved. Never throws.
[[nodiscard]] std::vector<Account> list();

// Accounts for one provider only.
[[nodiscard]] std::vector<Account> list_for(const std::string& provider);

// The label of the account currently marked active for `provider`, if the
// registry knows one. Empty when the provider has no registered accounts.
[[nodiscard]] std::string active_label(const std::string& provider);

// Insert or update the (provider, label) slot with a fresh secret, mark it
// active for that provider, and persist. Used right after a successful login
// to capture the just-authenticated credential under a name.
bool upsert(const std::string& provider, const std::string& label,
            const std::string& secret);

// Fetch a specific slot's secret. nullopt when absent.
[[nodiscard]] std::optional<Account> get(const std::string& provider,
                                         const std::string& label);

// Remove a slot. If it was the active one, the newest remaining account for
// that provider (if any) becomes active. Returns true if a slot was removed.
bool remove(const std::string& provider, const std::string& label);

// Record which label is active for a provider (without changing secrets).
bool set_active(const std::string& provider, const std::string& label);

// The registry file (config_dir()/accounts.json). Exposed for tests.
[[nodiscard]] std::string path();

// ── High-level per-provider adapters (account_switch.cpp) ────────────────
// These bridge the opaque registry to each provider's ACTIVE credential
// store. They know how to read the current live credential as a secret blob
// and how to install a saved blob back as the live credential.

// Read the provider's current active credential and store it in the registry
// under `label` (marking it active). Call right after a successful login so
// the just-authenticated account is captured by name. Returns false when the
// provider has no live credential to snapshot.
bool snapshot_active(const std::string& provider, const std::string& label);

// Install the saved (provider, label) credential as the provider's active
// credential and mark it active in the registry. Does NOT touch the live
// in-process auth header — the caller (reducer) re-installs that via
// make_auth_header so this module stays free of runtime/app deps. Returns
// false when the slot is absent or the install fails.
bool activate(const std::string& provider, const std::string& label);

// Best-effort human label for whatever credential is currently live in the
// provider's active store but not yet registered (legacy single-login case).
// Returns "" when nothing is signed in. Used to auto-register the pre-existing
// login as "default" the first time the account picker opens.
[[nodiscard]] std::string derive_current_label(const std::string& provider);

} // namespace agentty::auth::accounts
