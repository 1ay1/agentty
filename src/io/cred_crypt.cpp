#include "agentty/auth/cred_crypt.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include "agentty/util/base64.hpp"

#if defined(_WIN32)
#  include <windows.h>
#else
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
    if (!derive_key(machine_seed(), salt, sizeof(salt), key))
        return std::nullopt;

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
        {"v",     1},
        {"enc",   "aes-256-gcm"},
        {"salt",  b64(salt, sizeof(salt))},
        {"nonce", b64(nonce, sizeof(nonce))},
        {"ct",    b64(reinterpret_cast<const unsigned char*>(ct.data()), ct.size())},
        {"tag",   b64(tag, sizeof(tag))},
    };
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

    std::array<unsigned char, 32> key{};
    if (!derive_key(machine_seed(),
                    reinterpret_cast<const unsigned char*>(salt.data()),
                    salt.size(), key))
        return std::nullopt;

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

} // namespace agentty::auth::crypt
