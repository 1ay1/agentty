// web_search_secret.cpp — API keys for web_search services. See the header.

#include "agentty/tool/web_search_secret.hpp"

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keystore.hpp"
#include "agentty/io/shared_file.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <unordered_map>

#include <maya/runtime.hpp>

namespace agentty::tools::web_search_secret {

namespace {

namespace fs = std::filesystem;

// service -> key as last read from the keystore or sealed file.
using KeyCache = std::unordered_map<std::string, std::string>;
// A snapshot, not a guarded map: tiny, read per search, written rarely, and
// load()/store() are called from tool bodies that should not need a capability.
maya::snapshot<KeyCache>& key_cache() {
    static maya::snapshot<KeyCache> c;
    return c;
}
void forget(std::string_view service) {
    // Every store's entry for this service: cheap, and never stale.
    (void)key_cache().update([](const KeyCache& old, std::string s) {
        KeyCache c = old;
        const std::string tail = '\n' + s;
        for (auto it = c.begin(); it != c.end();)
            it = it->first.ends_with(tail) ? c.erase(it) : std::next(it);
        return c;
    }, std::string{service});
}

// One file for every service's key, sealed as a unit.
[[nodiscard]] fs::path secrets_path() {
    return auth::config_dir() / "web_search_keys.json";
}

[[nodiscard]] std::string keystore_key(std::string_view service) {
    return "agentty:web_search:" + std::string{service};
}

// The sealed file, opened. nullopt when a file IS there but cannot be read
// back -- corrupt, truncated, or sealed on another machine. That is not the
// same as "no keys": a store() that went ahead would rewrite the file with one
// key and silently destroy every other service's. So callers refuse to write.
[[nodiscard]] std::optional<nlohmann::json> parse_all(const std::optional<std::string>& file) {
    if (!file || file->empty()) return nlohmann::json::object();   // no keys yet
    const std::string& raw = *file;
    // Only a sealed envelope is accepted. Unlike embed_keys.json there is no
    // legacy plaintext form to read, so a hand-written plaintext file is not
    // trusted as a key store.
    if (auto opened = auth::crypt::unseal(raw)) {
        try {
            auto j = nlohmann::json::parse(*opened);
            if (j.is_object()) return j;
        } catch (...) { }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<nlohmann::json> read_all() {
    return parse_all(persistence::peek_file(secrets_path()));
}

// Set (empty key: drop) one service's key, as one read-modify-write in the
// file's shared-file section so two windows saving different services both
// keep theirs. Sealed, 0600, atomic. Refuses to touch an unreadable file
// (it would destroy the other services' keys) or to write plain.
persistence::Update update_key(Io io, const lock_levels::below_file_lane& cap,
                               const std::string& service, const std::string& key) {
    return persistence::update_shared(io, cap, secrets_path(),
        {.durability = persistence::Durability::Durable, .mode = 0600},
        [](std::optional<std::string> cur, std::string svc, std::string k)
            -> std::optional<std::string> {
            auto all = parse_all(cur);
            if (!all) return std::nullopt;
            if (k.empty()) { if (all->erase(svc) == 0) return std::nullopt; }
            else (*all)[svc] = k;
            return auth::crypt::seal(all->dump());
        }, service, key);
}

[[nodiscard]] std::string from_env(std::string_view service) {
    const std::string name = env_name(service);
    if (const char* v = std::getenv(name.c_str()); v && *v) return v;
    return {};
}

[[nodiscard]] std::string from_store(std::string_view service) {
    if (auth::keystore::available()) {
        std::string out;
        if (auth::keystore::retrieve(keystore_key(service), out)
                == auth::keystore::Status::Ok && !out.empty())
            return out;
    }
    const auto all = read_all();
    if (!all) return {};
    if (auto it = all->find(std::string{service}); it != all->end() && it->is_string())
        return it->get<std::string>();
    return {};
}

} // namespace

std::string env_name(std::string_view service) {
    std::string out = "AGENTTY_WEB_SEARCH_KEY_";
    for (char c : service)
        out.push_back(c == '-' ? '_'
                      : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

std::string load(std::string_view service) {
    if (service.empty()) return {};
    if (auto e = from_env(service); !e.empty()) return e;
    // A search reads up to three keys per call, and a store read may spawn
    // secret-tool. Keep what we read, per secrets file (a different
    // AGENTTY_HOME is a different store); store()/erase() drop the entry.
    const std::string ck = secrets_path().string() + '\n' + std::string{service};
    {
        const auto c = key_cache().load();
        if (auto it = c->find(ck); it != c->end()) return it->second;
    }
    std::string got = from_store(service);
    // Only a found key is kept: "none" must not hide one stored by another
    // agentty window later.
    if (!got.empty())
        (void)key_cache().update([](const KeyCache& old, std::string s, std::string k) {
            KeyCache c = old;
            c[std::move(s)] = std::move(k);
            return c;
        }, ck, got);
    return got;
}

bool has(std::string_view service) { return !load(service).empty(); }

std::string origin(std::string_view service) {
    if (service.empty()) return {};
    if (!from_env(service).empty()) return "env: " + env_name(service);
    if (!from_store(service).empty()) return "stored";
    return {};
}

bool plausible(std::string_view key) {
    if (key.size() < 8 || key.size() > 512) return false;
    for (char c : key) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;
    }
    return true;
}

bool store(Io io, const lock_levels::below_file_lane& cap, std::string_view service,
           std::string_view key) {
    if (service.empty()) return false;
    forget(service);
    if (key.empty()) return erase(io, cap, service);
    if (!plausible(key)) return false;

    if (auth::keystore::available()
        && auth::keystore::store(keystore_key(service), std::string{key})
               == auth::keystore::Status::Ok) {
        // One home for the secret: drop any stale sealed copy.
        (void)update_key(io, cap, std::string{service}, {});
        return true;
    }
    if (update_key(io, cap, std::string{service}, std::string{key})
        != persistence::Update::Written)
        return false;
    // The keystore is enabled but refused the write: an OLDER copy there would
    // still win over the file (load() asks the keystore first), so drop it.
    if (auth::keystore::available())
        (void)auth::keystore::remove(keystore_key(service));
    return true;
}

bool erase(Io io, const lock_levels::below_file_lane& cap, std::string_view service) {
    if (service.empty()) return false;
    forget(service);
    bool any = false;
    if (auth::keystore::available())
        any = auth::keystore::remove(keystore_key(service)) == auth::keystore::Status::Ok;
    if (update_key(io, cap, std::string{service}, {}) == persistence::Update::Written) any = true;
    return any;
}

} // namespace agentty::tools::web_search_secret
