# agentty — Security Audit & Hardening

Status: **all findings remediated**. Build clean, `55/55` ctest green, every
security-sensitive path verified end-to-end (not merely compiled). This
document is the record of what was audited, what was found, and what was
changed. The code is the source of truth; where this doc and the code
disagree, the code wins.

Scope of the audit: the security-critical surfaces only — workspace boundary,
sandbox, subprocess spawner, tool permission/effect model, credential storage
& crypto, TLS, and the OAuth/auth module. Method: **code-only** (docs and
comments were not trusted; every claim was verified against the source).

---

## 1. Findings & remediations

| # | Severity | Finding | Fix | Location |
|---|----------|---------|-----|----------|
| F1 | **HIGH** | PKCE `code_verifier` + OAuth `state` came from `std::mt19937_64` (non-CSPRNG), seeded from ~128 bits of `random_device`. MT19937 is recoverable from a few outputs. | Rewrote `random_urlsafe` to draw from OpenSSL **`RAND_bytes`**; unbiased 6-bit mask into the 64-char alphabet (`static_assert`-checked); throws on entropy failure rather than degrading. | `src/io/auth.cpp` `random_urlsafe()` |
| F2 | **MED** | Anti-CSRF `state` was generated but **never verified** — `exchange_code` split off the echoed state after `#` and discarded it. | `exchange_code` now parses the echoed state and does a **constant-time compare** against the expected value, failing closed on mismatch (only enforced when the IdP echoes a state; PKCE still binds otherwise). | `src/io/auth.cpp` `exchange_code()` |
| F3 | LOW | Credentials file was written in-place with `O_TRUNC` — a crash mid-write could corrupt it. | `write_private` now writes a sibling temp file, `fsync`s, and `rename`s atomically; created `0600` from `open()`. | `src/io/auth.cpp` `write_private()` |
| F4 | LOW | Config dir created with process umask — potentially world-listable. | `config_dir()` best-effort **`chmod 0700`** (POSIX). | `src/io/auth.cpp` `config_dir()` |
| F5 | LOW | No TLS certificate pinning (standard chain verification only). | **Opt-in SPKI pinning** via `AGENTTY_TLS_PINS` (HPKP-format base64 SHA-256 SPKI, comma-list with backups); `pin_verify_cb` layered on `SSL_VERIFY_PEER`. Off by default so a server key rotation can't brick clients. | `src/io/tls.cpp` |
| F6 | (design) | Credentials at-rest key was derived from machine-id + uid with **no user secret** — a same-uid local process can re-derive it. | Two independent hardening layers, both **opt-in**: (a) a memory-hard **passphrase factor** folded into the key; (b) **OS-native keystore** backing. See §2. | `src/io/cred_crypt.*`, `src/io/keystore.*` |
| — | robustness | CSPRNG failure in `random_urlsafe` could throw out of a *pure* reducer. | Guarded the PKCE mint in the login reducer with `try/catch → login::Failed` so the reducer stays total. | `src/runtime/app/update/login.cpp` |

---

## 2. Credential-at-rest hardening (F6)

The baseline (pre-audit) at-rest scheme: `credentials.json` sealed with
**AES-256-GCM**, key = `HKDF-SHA256(machine_seed, salt)`, fresh random
128-bit salt + 96-bit nonce per save. This defends against the file *leaving*
the machine (backup, cloud-sync, screenshot). It does **not** defend against a
local attacker running as the same uid. Two opt-in layers close that gap.

### 2a. Passphrase factor (memory-hard KDF)

When a passphrase is configured, the key becomes
`HKDF( machine_seed ‖ MHF(passphrase, salt) )` — decryption then requires
**both** the machine **and** the secret the user holds.

- **KDF selection (Argon2id → scrypt):** New files prefer **Argon2id**
  (t=3, m=64 MiB, p=1) when the linked OpenSSL provides it (≥ 3.2, probed at
  runtime via `EVP_KDF_fetch`). Older OpenSSL transparently falls back to
  **scrypt** (N=2¹⁵, r=8, p=1 — available since 3.0). `AGENTTY_KDF=scrypt`
  forces the fallback.
- **Envelope versioning:** passphrase files are **v2** and stamp
  `kdf` (`"argon2id"` | `"scrypt"`) plus the exact cost params. `unseal`
  dispatches on the stored KDF and reproduces the derivation from the
  **stored** params — so a file sealed with argon2id still opens on a build
  that later retunes costs, and a scrypt file opens on an argon2id-capable
  build. Machine-only files stay **v1** and keep decrypting under passphrase
  mode (backward compatible).
- **Fail-closed:** a v2 file with no passphrase → `nullopt`; wrong passphrase
  → GCM auth fails → `nullopt`.

**Enable:**
- `AGENTTY_PASSPHRASE=<secret>` — non-interactive / CI, or
- `AGENTTY_ENCRYPT_PASSPHRASE=1` — prompt once on `/dev/tty` with echo off.

The passphrase is resolved once per process (`std::call_once`), and scrypt /
argon2 intermediate key material is `OPENSSL_cleanse`d after use.

### 2b. OS keystore backing (opt-in)

When enabled, the OS's own vault holds the sealed envelope; the encrypted file
remains as a fallback (already machine-bound-encrypted, so no downgrade).

- **Backends** (runtime-detected, no new link-time deps):
  - **Linux** — libsecret via the `secret-tool` CLI (GNOME Keyring / KWallet
    through the Secret Service D-Bus API).
  - **macOS** — the `security` CLI (login keychain generic-password items).
  - **Windows** — Credential Manager via `CredWriteW`/`CredReadW`/
    `CredDeleteW` (per-user, DPAPI-protected generic credentials; links
    `advapi32`).
- **Enable:** `AGENTTY_USE_KEYSTORE=1` (off by default so no keychain-unlock
  prompt appears for users who don't want it).
- **Secret hygiene:** the libsecret `store` feeds the secret over **stdin**
  (never in argv/`ps`); lookups/deletes pass only the item label. macOS
  `add-generic-password -w` is invoked **with no value** so `security` prompts
  for the password on stdin (`readpassphrase`); the spawner detaches the child
  into its own session (`POSIX_SPAWN_SETSID`) so no controlling tty exists and
  `readpassphrase` falls back to the stdin pipe — the secret never reaches argv.
- **Integration:** `load_credentials` prefers the keystore then falls back to
  file; `save_credentials` writes both; `clear_credentials` (logout) removes
  the keystore item and deletes the file.

### Visibility

`agentty status` reports both layers, e.g.:

```
At-rest encryption: aes-256-gcm (machine + passphrase)
OS keystore: libsecret
```

or, with neither opt-in:

```
At-rest encryption: aes-256-gcm (machine-bound; set AGENTTY_PASSPHRASE or AGENTTY_ENCRYPT_PASSPHRASE=1 to add a passphrase)
OS keystore: disabled (set AGENTTY_USE_KEYSTORE=1 to enable)
```

---

## 3. Subsystems reviewed and found sound (no change needed)

| Subsystem | Verdict |
|-----------|---------|
| **Workspace boundary** (`src/tool/util/fs_helpers.cpp`) | Component-wise containment via `weakly_canonical` — symlink-aware, no naive prefix-string bug. File tools refuse paths outside cwd. |
| **Subprocess spawner** (`src/tool/util/subprocess.cpp`) | `posix_spawn`, stdin ← `/dev/null`, argv form bypasses the shell (no injection), idle-deadline watchdog, correct fd/close hygiene, byte caps. |
| **Permission model** (`include/agentty/tool/policy.hpp`, `effects.hpp`) | 4-bit `EffectSet` → constexpr Profile×Effect matrix, **exhaustively `static_assert`-proven at build time**. The same set drives parallel-scheduling exclusivity. |
| **Credential crypto core** (`src/io/cred_crypt.cpp`) | AES-256-GCM, HKDF-SHA256, random salt+nonce via `RAND_bytes`, GCM tag set before `DecryptFinal` (fails closed), keys `OPENSSL_cleanse`d. |
| **TLS chain verification** (`src/io/tls.cpp`) | Full chain + hostname (`SSL_set1_host`, no partial wildcards), TLS 1.2 floor, system roots per-platform. `AGENTTY_INSECURE=1` opt-out is sampled once and documented. |
| **`open_browser`** (`src/io/auth.cpp`) | `posix_spawnp`/`ShellExecuteA` with the URL as a distinct argv element — no `/bin/sh -c`, injection-safe by construction. |
| **Token refresh** (`src/io/auth.cpp`) | Correct expiry + 60 s skew, mutex-serialized across workers, refresh-token rotation handled. |

---

## 4. Configuration reference (all opt-in, off by default)

| Env var | Effect |
|---------|--------|
| `AGENTTY_TLS_PINS` | Comma-separated base64 SHA-256 SPKI pins. When set, the leaf cert must match one; sampled at first handshake (set before the first request). |
| `AGENTTY_PASSPHRASE` | Passphrase for at-rest key derivation (non-interactive). |
| `AGENTTY_ENCRYPT_PASSPHRASE=1` | Prompt for the passphrase once on `/dev/tty`. |
| `AGENTTY_KDF=scrypt` | Force scrypt instead of Argon2id (testing / parity). |
| `AGENTTY_USE_KEYSTORE=1` | Back the credentials with the OS keystore. |
| `AGENTTY_INSECURE=1` | **Disables** TLS verification. Debugging only. Sampled once per process. |

---

## 5. Verification performed

- **Build:** clean across all changed TUs (`auth.cpp`, `tls.cpp`,
  `cred_crypt.cpp`, `keystore.cpp`, `login.cpp`), 0 warnings.
- **Full suite:** `100% tests passed, 0 failed out of 57` — including two new
  CI-covered unit tests:
  - `cred_crypt_test` — forks per passphrase state to cover v1 machine-only
    (+ tamper rejection + plaintext detection), v2/argon2id, v2/scrypt, and
    the fail-closed paths (no / wrong passphrase).
  - `keystore_test` — store/retrieve/remove/NotFound round-trip; skips with
    success when no backend is available (headless CI).
- **TLS pin matrix** (live, against `api.anthropic.com`): no-pin OK ·
  wrong-pin **rejected** · correct-pin OK · correct-pin-among-list-with-decoy OK.
- **At-rest crypto matrix**: v1 (machine-only) round-trips · v2/argon2id
  round-trips · v2/scrypt round-trips · v1 opens under passphrase mode ·
  argon2id file reopens · scrypt file reopens on argon2id-capable build ·
  v2-without-passphrase refused · v2-wrong-passphrase refused.
- **Keystore round-trip** (live, libsecret): store → retrieve (exact match)
  → remove → retrieve-after-remove = NotFound, no keyring residual.

---

## 6. Residual risk / future work

- The machine-only default remains vulnerable to a same-uid local attacker
  *unless* the passphrase and/or keystore opt-ins are enabled. This is a
  deliberate zero-friction default; the hardening is one env var away.
