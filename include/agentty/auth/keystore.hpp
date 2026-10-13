#pragma once
// agentty::auth::keystore — optional OS-native secret storage.
//
// SECURITY_AUDIT long-term recommendation: back the credentials file with
// the operating system's own vault so the token never sits on disk in a form
// any same-user process can decrypt. The machine-bound (and optional
// passphrase) at-rest encryption in cred_crypt.* stays as defense in depth;
// when the keystore is enabled it holds the sealed envelope and the on-disk
// file becomes a cache/fallback only.
//
// Backends (chosen at runtime, no new link-time deps):
//   Linux   — libsecret via the `secret-tool` CLI (GNOME Keyring / KWallet
//             through the Secret Service D-Bus API). Probed for presence.
//   macOS   — the `security` CLI (login keychain, generic password items).
//   Windows — not yet implemented; retrieve/store report Unsupported and the
//             caller transparently falls back to the encrypted file.
//
// Opt-in: only consulted when AGENTTY_USE_KEYSTORE is truthy. Off by default
// so existing installs are unchanged and no keychain-unlock prompt appears
// for users who don't want it.

#include <optional>
#include <string>

namespace agentty::auth::keystore {

// Outcome of a keystore operation. `Unsupported` means there is no usable
// backend on this platform/build (caller should fall back to the file);
// `NotFound` means the backend works but holds no item under `key`; `Error`
// means the backend was present but the operation failed.
enum class Status { Ok, NotFound, Unsupported, Error };

// True iff a keystore backend is BOTH available on this system AND enabled
// via AGENTTY_USE_KEYSTORE. Cheap (probes once, caches). Callers gate their
// store/retrieve on this.
bool available();

// Human label for the active backend ("libsecret", "macos-keychain",
// "disabled", "unavailable") — for `agentty status`.
std::string backend_name();

// Store `secret` under `key` (created or replaced). Returns Ok on success.
Status store(const std::string& key, const std::string& secret);

// Retrieve the secret stored under `key`. On Ok, `out` holds the value.
Status retrieve(const std::string& key, std::string& out);

// Remove any secret stored under `key`. Ok even if nothing was present is
// acceptable; NotFound is also fine for callers doing a best-effort wipe.
Status remove(const std::string& key);

} // namespace agentty::auth::keystore
