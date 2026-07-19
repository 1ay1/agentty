#include "agentty/auth/cred_crypt.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include "agentty/util/base64.hpp"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace agentty::auth::crypt {

namespace {

using json = nlohmann::json;

// Fixed application context bound into the key derivation. Changing this
// string invalidates every previously-sealed file (they'll fail auth and
// be treated as unrecoverable → the user re-runs `agentty login`).
constexpr std::string_view kInfo = "agentty-credentials-v1";

// ── Machine-stable seed ────────────────────────────────────────────────
// Mixes: a per-machine id, the current user id, and the app context. This
// is NOT a secret store — it's key material that stays constant for the
// same (machine, user) so a file sealed today decrypts tomorrow, but a
// copy of the file taken to ANOTHER machine (or another user) won't.
std::string machine_seed() {
    std::string seed;

#if defined(_WIN32)
    // MachineGuid is a stable per-install identifier.
    HKEY hk{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography", 0,
                      KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        char buf[256]; DWORD sz = sizeof(buf); DWORD type = 0;
        if (RegQueryValueExA(hk, "MachineGuid", nullptr, &type,
                             reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS
            && type == REG_SZ && sz > 0) {
            seed.assign(buf, sz - 1); // drop trailing NUL
        }
        RegCloseKey(hk);
    }
    if (seed.empty()) {
        char name[256]; DWORD n = sizeof(name);
        if (GetComputerNameA(name, &n)) seed.assign(name, n);
    }
    // Per-user salt: username is a reasonable stand-in for uid on Windows.
    if (const char* u = std::getenv("USERNAME")) { seed += '\x1f'; seed += u; }
#else
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::ifstream f(path);
        if (f) { std::getline(f, seed); if (!seed.empty()) break; }
    }
    if (seed.empty()) {
        char host[256];
        if (::gethostname(host, sizeof(host)) == 0) {
            host[sizeof(host) - 1] = '\0';
            seed = host;
        }
    }
    // Bind to the numeric uid so a shared machine keeps per-user isolation.
    seed += '\x1f';
    seed += std::to_string(static_cast<unsigned long>(::getuid()));
#endif

    if (seed.empty()) seed = "agentty-fallback-seed";
    seed += '\x1f';
    seed.append(kInfo);
    return seed;
}

// HKDF-SHA256(seed, salt, info) → 32-byte AES key. OpenSSL 3's EVP_KDF is
// the portable path; the deprecated one-shot HKDF() would also work but
// EVP_KDF is what's guaranteed present in the linked OpenSSL.
bool derive_key(const std::string& seed,
                const unsigned char* salt, size_t salt_len,
                std::array<unsigned char, 32>& out_key) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (!kdf) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return false;

    OSSL_PARAM params[5];
    int i = 0;
    char digest[] = "SHA256";
    params[i++] = OSSL_PARAM_construct_utf8_string("digest", digest, 0);
    params[i++] = OSSL_PARAM_construct_octet_string(
        "key", const_cast<char*>(seed.data()), seed.size());
    params[i++] = OSSL_PARAM_construct_octet_string(
        "salt", const_cast<unsigned char*>(salt), salt_len);
    params[i++] = OSSL_PARAM_construct_octet_string(
        "info", const_cast<char*>(kInfo.data()), kInfo.size());
    params[i]   = OSSL_PARAM_construct_end();

    bool ok = EVP_KDF_derive(ctx, out_key.data(), out_key.size(), params) == 1;
    EVP_KDF_CTX_free(ctx);
    return ok;
}

// ── Optional passphrase (memory-hard factor) ───────────────────────────
// scrypt parameters. N=2^15 (32768), r=8, p=1 is the interactive-login
// profile from RFC 7914 — ~32 MiB and tens of ms per derivation, which
// makes an offline guessing attack against the file painfully slow while
// staying imperceptible for the once-per-save/load path here. Stored in
// the envelope so unseal reproduces them even if we retune later.
constexpr uint64_t kScryptN = 1u << 15;
constexpr uint32_t kScryptR = 8;
constexpr uint32_t kScryptP = 1;

// Read a line from /dev/tty with terminal echo disabled (so the passphrase
// doesn't paint on screen). Returns std::nullopt if there is no controlling
// tty (e.g. piped/headless) — the caller then errors instead of prompting.
std::optional<std::string> prompt_passphrase() {
#if defined(_WIN32)
    return std::nullopt;   // interactive prompt unsupported here; use env var
#else
    FILE* tty = std::fopen("/dev/tty", "r+");
    if (!tty) return std::nullopt;
    int fd = ::fileno(tty);

    termios old{};
    bool restored = false;
    if (::tcgetattr(fd, &old) == 0) {
        termios noecho = old;
        noecho.c_lflag &= ~(tcflag_t)ECHO;
        ::tcsetattr(fd, TCSAFLUSH, &noecho);
    } else {
        restored = true;   // couldn't toggle echo; still read, just echoes
    }

    std::fputs("Enter agentty credential passphrase: ", tty);
    std::fflush(tty);

    std::string line;
    int c;
    while ((c = std::fgetc(tty)) != EOF && c != '\n') line.push_back((char)c);

    if (!restored) { ::tcsetattr(fd, TCSAFLUSH, &old); std::fputc('\n', tty); }
    std::fclose(tty);
    return line;
#endif
}

// Resolve the passphrase for this process, ONCE. Precedence:
//   1. AGENTTY_PASSPHRASE (explicit secret, e.g. CI)
//   2. AGENTTY_ENCRYPT_PASSPHRASE truthy → interactive prompt on /dev/tty
//   3. neither → no passphrase (machine-only mode)
// The result (present-or-absent) is cached so we prompt at most once per run
// and every seal/unseal in the session agrees.
const std::optional<std::string>& resolve_passphrase() {
    static std::once_flag once;
    static std::optional<std::string> cached;
    std::call_once(once, [] {
        if (const char* p = std::getenv("AGENTTY_PASSPHRASE"); p && *p) {
            cached = std::string{p};
            return;
        }
        const char* want = std::getenv("AGENTTY_ENCRYPT_PASSPHRASE");
        const bool truthy = want && *want
            && std::strcmp(want, "0") != 0
            && std::strcmp(want, "false") != 0;
        if (truthy) {
            if (auto pw = prompt_passphrase(); pw && !pw->empty())
                cached = std::move(pw);
        }
    });
    return cached;
}

// Fold a passphrase into 32 bytes of memory-hard key material via scrypt,
// with EXPLICIT cost params. Same (passphrase, salt, params) always yields
// the same output, so unseal reproduces it from the envelope's stored params.
// Returns false on OpenSSL failure (e.g. memory limit).
bool scrypt_derive_params(const std::string& pass,
                          const unsigned char* salt, size_t salt_len,
                          uint64_t n, uint32_t r, uint32_t p,
                          std::array<unsigned char, 32>& out) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "SCRYPT", nullptr);
    if (!kdf) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return false;

    // scrypt's default memory guard (~32 MiB) can reject N=2^15 on some
    // builds; raise the ceiling so the intended params always run.
    uint64_t maxmem = 256ull * 1024 * 1024;
    OSSL_PARAM full[7];
    int i = 0;
    full[i++] = OSSL_PARAM_construct_octet_string(
        "pass", const_cast<char*>(pass.data()), pass.size());
    full[i++] = OSSL_PARAM_construct_octet_string(
        "salt", const_cast<unsigned char*>(salt), salt_len);
    full[i++] = OSSL_PARAM_construct_uint64("n", &n);
    full[i++] = OSSL_PARAM_construct_uint32("r", &r);
    full[i++] = OSSL_PARAM_construct_uint32("p", &p);
    full[i++] = OSSL_PARAM_construct_uint64("maxmem_bytes", &maxmem);
    full[i]   = OSSL_PARAM_construct_end();

    bool ok = EVP_KDF_derive(ctx, out.data(), out.size(), full) == 1;
    EVP_KDF_CTX_free(ctx);
    return ok;
}

// Convenience: derive with the current default params (used at seal time).
bool scrypt_derive(const std::string& pass,
                   const unsigned char* salt, size_t salt_len,
                   std::array<unsigned char, 32>& out) {
    return scrypt_derive_params(pass, salt, salt_len,
                                kScryptN, kScryptR, kScryptP, out);
}

// ── Argon2id (preferred when the linked OpenSSL provides it) ─────────────
// Argon2id is the modern PHC-winner memory-hard KDF; it resists both GPU and
// side-channel attacks better than scrypt. It only landed in OpenSSL 3.2, so
// we PROBE for it at runtime and fall back to scrypt on older libraries. New
// files prefer argon2id; the envelope records which KDF ran so unseal always
// reproduces the right one regardless of what the current build supports.
//
// Params: t=3 passes, m=65536 KiB (64 MiB), p=1 lane. A conservative
// interactive profile — ~64 MiB and tens of ms, matching scrypt's cost class
// while being meaningfully harder to attack offline.
constexpr uint32_t kArgonT = 3;         // iterations / passes
constexpr uint32_t kArgonM = 64 * 1024; // memory in KiB (64 MiB)
constexpr uint32_t kArgonP = 1;         // parallelism / lanes

bool argon2id_available() {
    static const bool ok = [] {
        EVP_KDF* k = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
        if (!k) return false;
        EVP_KDF_free(k);
        return true;
    }();
    return ok;
}

// Derive 32 bytes via Argon2id with EXPLICIT params (so unseal reproduces
// from the envelope). Returns false on OpenSSL failure / unsupported.
bool argon2id_derive_params(const std::string& pass,
                            const unsigned char* salt, size_t salt_len,
                            uint32_t t, uint32_t m, uint32_t p,
                            std::array<unsigned char, 32>& out) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!kdf) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return false;

    OSSL_PARAM full[7];
    int i = 0;
    full[i++] = OSSL_PARAM_construct_octet_string(
        "pass", const_cast<char*>(pass.data()), pass.size());
    full[i++] = OSSL_PARAM_construct_octet_string(
        "salt", const_cast<unsigned char*>(salt), salt_len);
    full[i++] = OSSL_PARAM_construct_uint32("iter",      &t);
    full[i++] = OSSL_PARAM_construct_uint32("memcost",   &m);
    full[i++] = OSSL_PARAM_construct_uint32("lanes",     &p);
    // Argon2 requires threads <= lanes; pin threads=1 for determinism.
    static uint32_t threads = 1;
    full[i++] = OSSL_PARAM_construct_uint32("threads",   &threads);
    full[i]   = OSSL_PARAM_construct_end();

    bool ok = EVP_KDF_derive(ctx, out.data(), out.size(), full) == 1;
    EVP_KDF_CTX_free(ctx);
    return ok;
}

// Produce the final 32-byte AES key for a given salt. When a passphrase is
// configured, the machine seed is EXTENDED with a memory-hard KDF over the
// passphrase so the key depends on BOTH factors; otherwise it's the machine-
// only path. `chosen_kdf` reports which passphrase KDF ran ("" = none, i.e.
// machine-only v1) so seal can stamp the envelope and unseal can reproduce it.
bool derive_final_key(const unsigned char* salt, size_t salt_len,
                      bool require_passphrase,
                      std::array<unsigned char, 32>& out_key,
                      std::string& chosen_kdf) {
    chosen_kdf.clear();
    std::string seed = machine_seed();

    const auto& pass = resolve_passphrase();
    if (pass && !pass->empty()) {
        std::array<unsigned char, 32> pk{};
        // Prefer Argon2id where available; fall back to scrypt on older
        // OpenSSL. AGENTTY_KDF=scrypt forces the fallback (testing / parity).
        const char* force = std::getenv("AGENTTY_KDF");
        const bool want_scrypt = force && std::strcmp(force, "scrypt") == 0;
        if (!want_scrypt && argon2id_available()
            && argon2id_derive_params(*pass, salt, salt_len,
                                      kArgonT, kArgonM, kArgonP, pk)) {
            chosen_kdf = "argon2id";
        } else if (scrypt_derive(*pass, salt, salt_len, pk)) {
            chosen_kdf = "scrypt";
        } else {
            return false;
        }
        // Bind the KDF output into the HKDF input keying material so the
        // final key needs machine seed AND passphrase.
        seed.push_back('\x1f');
        seed.append(reinterpret_cast<const char*>(pk.data()), pk.size());
        OPENSSL_cleanse(pk.data(), pk.size());
    } else if (require_passphrase) {
        // Envelope was sealed WITH a passphrase but none is available now.
        return false;
    }

    bool ok = derive_key(seed, salt, salt_len, out_key);
    OPENSSL_cleanse(seed.data(), seed.size());
    return ok;
}

bool rand_bytes(unsigned char* buf, size_t len) {
    return RAND_bytes(buf, static_cast<int>(len)) == 1;
}

std::string b64(const unsigned char* p, size_t n) {
    return util::base64_encode(p, n);
}

} // namespace

bool looks_sealed(const std::string& s) noexcept {
    // Cheap structural probe: a sealed file is a JSON object whose first
    // non-space bytes open a brace and mention our marker. Avoid a full
    // parse on the hot path (every load).
    auto pos = s.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos || s[pos] != '{') return false;
    return s.find("\"enc\"") != std::string::npos
        && s.find("aes-256-gcm") != std::string::npos;
}

std::optional<std::string> seal(const std::string& plaintext) {
    unsigned char salt[16];
    unsigned char nonce[12];
    if (!rand_bytes(salt, sizeof(salt)) || !rand_bytes(nonce, sizeof(nonce)))
        return std::nullopt;

    std::array<unsigned char, 32> key{};
    std::string chosen_kdf;
    if (!derive_final_key(salt, sizeof(salt), /*require_passphrase=*/false,
                          key, chosen_kdf))
        return std::nullopt;
    const bool used_pass = !chosen_kdf.empty();

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::string ct;
    unsigned char tag[16];
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), nullptr) != 1)
            break;
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1)
            break;

        ct.resize(plaintext.size());
        int outl = 0;
        if (EVP_EncryptUpdate(ctx,
                reinterpret_cast<unsigned char*>(ct.data()), &outl,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                static_cast<int>(plaintext.size())) != 1)
            break;
        int total = outl;
        int finl = 0;
        // GCM is a stream cipher: EncryptFinal emits no extra bytes, but
        // the call must run to finalize the tag.
        if (EVP_EncryptFinal_ex(ctx,
                reinterpret_cast<unsigned char*>(ct.data()) + total, &finl) != 1)
            break;
        total += finl;
        ct.resize(static_cast<size_t>(total));
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) != 1)
            break;
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    // Wipe the derived key from the stack promptly.
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) return std::nullopt;

    json env = {
        {"v",     used_pass ? 2 : 1},
        {"enc",   "aes-256-gcm"},
        {"salt",  b64(salt, sizeof(salt))},
        {"nonce", b64(nonce, sizeof(nonce))},
        {"ct",    b64(reinterpret_cast<const unsigned char*>(ct.data()), ct.size())},
        {"tag",   b64(tag, sizeof(tag))},
    };
    // When a passphrase factor is present, record the KDF + its params so a
    // future unseal (possibly after we retune the cost, or on a build without
    // argon2id) reproduces the exact derivation, and so unseal knows to
    // REQUIRE the passphrase.
    if (used_pass) {
        env["kdf"] = chosen_kdf;
        if (chosen_kdf == "argon2id") {
            env["kdf_t"] = kArgonT;   // passes
            env["kdf_m"] = kArgonM;   // memory KiB
            env["kdf_p"] = kArgonP;   // lanes
        } else {  // scrypt
            env["kdf_n"] = kScryptN;
            env["kdf_r"] = kScryptR;
            env["kdf_p"] = kScryptP;
        }
    }
    return env.dump();
}

std::optional<std::string> unseal(const std::string& envelope) {
    json j;
    try {
        j = json::parse(envelope);
    } catch (...) {
        return std::nullopt;   // not JSON → not our envelope
    }
    if (!j.is_object() || j.value("enc", "") != "aes-256-gcm")
        return std::nullopt;

    std::string salt, nonce, ct, tag;
    try {
        salt  = util::base64_decode(j.value("salt", ""));
        nonce = util::base64_decode(j.value("nonce", ""));
        ct    = util::base64_decode(j.value("ct", ""));
        tag   = util::base64_decode(j.value("tag", ""));
    } catch (...) {
        return std::nullopt;
    }
    if (salt.empty() || nonce.size() != 12 || tag.size() != 16)
        return std::nullopt;

    // Does this envelope carry a passphrase factor? v2 envelopes stamp a
    // "kdf" (argon2id or scrypt) plus the exact cost params. If so, the
    // passphrase is REQUIRED and we reproduce the derivation with the STORED
    // params + KDF (not the current defaults — the sealing build may have
    // used a different KDF or cost than this one).
    const std::string kdf_name = j.value("kdf", "");
    const bool has_pass_kdf = (kdf_name == "scrypt" || kdf_name == "argon2id");

    std::array<unsigned char, 32> key{};
    const auto* salt_p = reinterpret_cast<const unsigned char*>(salt.data());

    if (has_pass_kdf) {
        const auto& pass = resolve_passphrase();
        if (!pass || pass->empty())
            return std::nullopt;   // sealed with a passphrase; none available

        std::array<unsigned char, 32> pk{};
        bool kok = false;
        if (kdf_name == "argon2id") {
            kok = argon2id_derive_params(*pass, salt_p, salt.size(),
                                         j.value("kdf_t", (uint32_t)kArgonT),
                                         j.value("kdf_m", (uint32_t)kArgonM),
                                         j.value("kdf_p", (uint32_t)kArgonP),
                                         pk);
        } else {  // scrypt
            kok = scrypt_derive_params(*pass, salt_p, salt.size(),
                                       j.value("kdf_n", (uint64_t)kScryptN),
                                       j.value("kdf_r", (uint32_t)kScryptR),
                                       j.value("kdf_p", (uint32_t)kScryptP),
                                       pk);
        }
        if (!kok) return std::nullopt;   // KDF failed / argon2id unsupported here

        std::string seed = machine_seed();
        seed.push_back('\x1f');
        seed.append(reinterpret_cast<const char*>(pk.data()), pk.size());
        OPENSSL_cleanse(pk.data(), pk.size());
        bool dk = derive_key(seed, salt_p, salt.size(), key);
        OPENSSL_cleanse(seed.data(), seed.size());
        if (!dk) return std::nullopt;
    } else {
        // Legacy/machine-only envelope: derive from the machine seed alone.
        if (!derive_key(machine_seed(), salt_p, salt.size(), key))
            return std::nullopt;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;

    std::string pt;
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                static_cast<int>(nonce.size()), nullptr) != 1)
            break;
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(),
                reinterpret_cast<const unsigned char*>(nonce.data())) != 1)
            break;

        pt.resize(ct.size());
        int outl = 0;
        if (EVP_DecryptUpdate(ctx,
                reinterpret_cast<unsigned char*>(pt.data()), &outl,
                reinterpret_cast<const unsigned char*>(ct.data()),
                static_cast<int>(ct.size())) != 1)
            break;
        int total = outl;
        // Set the expected tag BEFORE DecryptFinal — that's where GCM
        // verifies authenticity. A tampered file / wrong machine key
        // fails here and returns nullopt (no plaintext leaks).
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                static_cast<int>(tag.size()),
                const_cast<char*>(tag.data())) != 1)
            break;
        int finl = 0;
        if (EVP_DecryptFinal_ex(ctx,
                reinterpret_cast<unsigned char*>(pt.data()) + total, &finl) != 1)
            break;   // authentication failed
        total += finl;
        pt.resize(static_cast<size_t>(total));
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) return std::nullopt;
    return pt;
}

bool passphrase_active() {
    const auto& p = resolve_passphrase();
    return p.has_value() && !p->empty();
}

} // namespace agentty::auth::crypt
