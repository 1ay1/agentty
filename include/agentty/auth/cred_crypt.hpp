#pragma once
// agentty::auth::crypt — at-rest encryption for the credentials file.
//
// SECURITY_AUDIT.md finding #1 (MEDIUM): OAuth/API tokens were stored as
// plaintext JSON in ~/.config/agentty/credentials.json (0600). A backup,
// a synced dotfiles repo, a shoulder-surf of the file, or any process that
// can read the user's home dir then walks away with a live token.
//
// This module wraps the JSON payload in an authenticated-encryption
// envelope so the on-disk bytes are opaque. It is DELIBERATELY not a
// substitute for an OS keystore (libsecret/Keychain/DPAPI — the audit's
// long-term recommendation): the key is DERIVED from machine-stable
// material, not stored in a hardware/OS vault, so a determined attacker
// who can also run code as the same user on the same machine can
// re-derive it. What it DOES defend against, at zero new dependency and
// zero UX cost, is the far more common exposure: the file leaving the
// machine (backup, cloud-sync, screenshot, `cat`) still readable.
//
// Envelope (JSON):
//   { "v":1, "enc":"aes-256-gcm",
//     "salt":<b64>, "nonce":<b64>, "ct":<b64>, "tag":<b64> }
//
// Crypto: AES-256-GCM (OpenSSL EVP — already linked). The 256-bit key is
// HKDF-SHA256(machine_seed, salt) where machine_seed mixes a per-machine
// id (/etc/machine-id, hostname, …), the numeric user id, and a fixed
// app-specific context string. A fresh random 128-bit salt + 96-bit nonce
// are generated on every save, so re-encrypting identical plaintext never
// produces identical ciphertext.
//
// ── Optional passphrase mode (SECURITY_AUDIT hardening) ──────────────────
// The machine-only key above does NOT protect against a local attacker who
// can run code as the SAME user (they re-derive the same seed). Set a
// passphrase to close that gap: the key then also folds in a memory-hard
// scrypt(passphrase, salt) factor, so the file can't be decrypted without
// BOTH the machine AND the secret the user holds in their head.
//
// Enable by either:
//   • exporting AGENTTY_PASSPHRASE=… (non-interactive / CI), or
//   • exporting AGENTTY_ENCRYPT_PASSPHRASE=1 to be prompted once on /dev/tty.
// When neither is set, behaviour is exactly the machine-only path (v1) and
// existing files keep working. Passphrase envelopes are v2 and carry the
// scrypt params so unseal can reproduce the derivation.

#include <optional>
#include <string>

namespace agentty::auth::crypt {

// True iff a passphrase has been configured for this process (env var set,
// or an interactive prompt has been answered). Cheap; used by callers that
// want to tell the user their vault is passphrase-protected.
bool passphrase_active();

// Encrypt `plaintext` into the JSON envelope described above. Returns
// std::nullopt only on a hard OpenSSL failure (out of memory / broken
// build) — callers treat that as "couldn't secure the file" and fall
// back to refusing to persist rather than writing plaintext.
std::optional<std::string> seal(const std::string& plaintext);

// Decrypt a JSON envelope produced by seal(). Returns std::nullopt if the
// input isn't a v1 envelope (e.g. a legacy plaintext credentials.json —
// the caller then tries to parse it directly and re-encrypts on next
// save), or if authentication fails (tampered / wrong machine / corrupt).
std::optional<std::string> unseal(const std::string& envelope);

// True iff `s` looks like a sealed envelope (starts a JSON object with a
// "v"/"enc" marker). Cheap structural probe used to branch load between
// the encrypted path and the legacy-plaintext migration path.
bool looks_sealed(const std::string& s) noexcept;

} // namespace agentty::auth::crypt
