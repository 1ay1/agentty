// agentty::auth::keys — impl of the sealed provider-key vault. See
// keys.hpp for the security rationale.
//
// Persistence mirrors auth.cpp's credentials store exactly: seal the JSON
// payload with crypt::seal (machine-bound AES-256-GCM), mirror the sealed
// envelope into the OS keystore when enabled, then write the file
// atomically at 0600. Load prefers the keystore (it holds the same
// envelope) and falls back to the file; a legacy plaintext body is never
// written back — the caller migrates it out of settings.json instead.

#include "agentty/auth/keys.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"        // config_dir()
#include "agentty/auth/cred_crypt.hpp"  // crypt::seal / unseal / looks_sealed
#include "agentty/auth/keystore.hpp"
#include "agentty/io/shared_file.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::auth::keys {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace crypt = agentty::auth::crypt;

fs::path vault_path() {
    return agentty::auth::config_dir() / "provider-keys.json";
}

// Atomic + 0600 write of the sealed envelope. Its only writer is
// save_settings_locked, inside the settings file's section, so concurrent
// processes already take turns here.
bool write_private(const fs::path& p, const std::string& body) {
    if (persistence::detail::write_in_section(p.string(), body,
            {.durability = persistence::Durability::Durable, .mode = 0600}))
        return true;
    util::dbglog("keys.save", "could not write " + p.string());
    return false;
}

// The vault body is a plain JSON object: { "provider-id": "key", … }.
// Reject non-string members rather than trusting them — the map is the
// wire contract with Settings.provider_keys.
std::optional<KeyMap> parse(const std::string& body) {
    try {
        json j = json::parse(body);
        if (!j.is_object()) return std::nullopt;
        KeyMap out;
        for (auto& [k, v] : j.items())
            if (v.is_string()) out[k] = v.get<std::string>();
        return out;
    } catch (const std::exception& e) {
        util::dbglog("keys.load.parse", e.what());
    } catch (...) {
        util::dbglog("keys.load.parse", "non-std exception");
    }
    return std::nullopt;
}

} // namespace

std::string path() { return vault_path().string(); }

KeyMap load() {
    // Keystore first, exactly like auth::load_credentials: when enabled it
    // holds the same sealed envelope, and the file becomes the fallback.
    if (keystore::available()) {
        std::string raw;
        if (keystore::retrieve("provider-keys", raw) == keystore::Status::Ok
            && !raw.empty()) {
            if (auto body = crypt::unseal(raw))
                if (auto parsed = parse(*body)) return std::move(*parsed);
            // A keystore envelope that fails to authenticate falls through
            // to the file — unseal is fail-closed, so this can only mean a
            // stale machine binding, and the file copy is the newer store.
        }
    }
    auto file = persistence::peek_file(vault_path());
    if (!file || file->empty()) return {};
    const std::string& raw = *file;
    // Fail closed on unauthenticated bytes: wrong machine / corrupt → {}.
    auto body = crypt::looks_sealed(raw) ? crypt::unseal(raw) : std::nullopt;
    if (!body) {
        util::dbglog("keys.load", "unauthenticated vault ignored");
        return {};
    }
    auto parsed = parse(*body);
    return parsed ? std::move(*parsed) : KeyMap{};
}

bool save(const KeyMap& keys) {
    // An empty map is AUTHORITATIVE (a sign-out must leave no recoverable
    // key at rest), so it wipes the vault rather than writing an empty
    // envelope — no ghost file, no stale keystore item.
    if (keys.empty()) {
        clear();
        return true;
    }
    json j = json::object();
    for (const auto& [k, v] : keys) j[k] = v;
    // Refuse to persist what we can't encrypt — same contract as
    // auth::save_credentials. The in-memory Settings still work for the
    // session; the caller surfaces the write failure.
    auto sealed = crypt::seal(j.dump(2));
    if (!sealed) {
        util::dbglog("keys.save", "seal failed; vault left unchanged");
        return false;
    }
    if (keystore::available())
        keystore::store("provider-keys", *sealed);
    return write_private(vault_path(), *sealed);
}

void clear() {
    if (keystore::available())
        keystore::remove("provider-keys");
    std::error_code ec;
    fs::remove(vault_path(), ec);
}

} // namespace agentty::auth::keys
