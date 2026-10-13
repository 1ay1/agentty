#pragma once
// agentty::auth::keys — the sealed at-rest vault for provider API keys.
//
// SECURITY_AUDIT (credential homogenisation): hosted-preset keys ("openai",
// "groq", …) AND custom-host keys (host[:port] / URL specs — including the
// keyless localhost rows, which are persisted as empty values so the picker
// has a row next session) used to rest PLAINTEXT in settings.json under
// "provider_keys", while credentials.json and accounts.json were sealed
// with crypt::seal. A backup, a synced dotfiles repo, a screen-share or a
// bug-report attachment of settings.json walked away with every live key.
//
// Every provider key now persists through THIS module into
// <credentials>/provider-keys.json — the same machine-bound AES-256-GCM
// envelope credentials.json uses, mirrored into the OS keystore when
// AGENTTY_USE_KEYSTORE is enabled (same keystore-first + file-fallback
// pattern as auth::save/load_credentials). settings.json stops carrying
// keys entirely; on first load a legacy plaintext "provider_keys" object
// is imported into the vault and stripped from settings.json (see
// persistence::load_settings for the one-time migration).
//
// The RUNTIME story is unchanged: store::Settings.provider_keys stays the
// in-memory source of truth (selection, picker rows, resolve() all read
// it); only the at-rest layer moves. An empty map saved is authoritative
// (a sign-out must be able to clear the vault), so save() always writes —
// there is no "skip empty" short-circuit.

#include <map>
#include <string>

namespace agentty::auth::keys {

// provider id (or custom-host spec) → key. Same shape as
// store::Settings::provider_keys, so the two convert directly.
using KeyMap = std::map<std::string, std::string>;

// On-disk location: <credentials>/provider-keys.json, next to the
// credentials.json / accounts.json stores it now matches.
[[nodiscard]] std::string path();

// Read the vault: keystore first (when enabled), sealed file as fallback.
// Returns {} when nothing is stored — or when the envelope can't be
// authenticated (wrong machine / corrupt), mirroring unseal()'s
// fail-closed contract: no keys, never a guess.
[[nodiscard]] KeyMap load();

// Seal `keys` and persist (keystore mirror + atomic 0600 file write).
// Returns false when sealing or the write fails — callers should surface
// that rather than silently falling back to plaintext.
bool save(const KeyMap& keys);

// Best-effort wipe: keystore item + sealed file.
void clear();

} // namespace agentty::auth::keys
