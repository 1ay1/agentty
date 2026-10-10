// web_search_secret.cpp — API keys for web_search services. See the header.

#include "agentty/tool/web_search_secret.hpp"

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keystore.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace agentty::tools::web_search_secret {

namespace {

namespace fs = std::filesystem;

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
[[nodiscard]] std::optional<nlohmann::json> read_all() {
    std::ifstream in(secrets_path(), std::ios::binary);
    if (!in) return nlohmann::json::object();          // no file: no keys yet
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    if (raw.empty()) return nlohmann::json::object();
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

bool write_all(const nlohmann::json& j) {
    std::error_code ec;
    fs::create_directories(secrets_path().parent_path(), ec);
    // Refuse to persist rather than fall back to plaintext.
    auto sealed = auth::crypt::seal(j.dump());
    if (!sealed) return false;

    const auto tmp = fs::path{secrets_path().string() + ".tmp"};
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << *sealed;
        if (!out) return false;
    }
#if !defined(_WIN32)
    // Owner-only before the rename, so the secret is never briefly readable.
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    fs::rename(tmp, secrets_path(), ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
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
    return from_store(service);
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

bool store(std::string_view service, std::string_view key) {
    if (service.empty()) return false;
    if (key.empty()) return erase(service);
    if (!plausible(key)) return false;

    if (auth::keystore::available()
        && auth::keystore::store(keystore_key(service), std::string{key})
               == auth::keystore::Status::Ok) {
        // One home for the secret: drop any stale sealed copy.
        if (auto all = read_all(); all && all->erase(std::string{service}) > 0)
            (void)write_all(*all);
        return true;
    }
    auto all = read_all();
    if (!all) return false;   // unreadable file: never overwrite the other keys
    (*all)[std::string{service}] = std::string{key};
    if (!write_all(*all)) return false;
    // The keystore is enabled but refused the write: an OLDER copy there would
    // still win over the file (load() asks the keystore first), so drop it.
    if (auth::keystore::available())
        (void)auth::keystore::remove(keystore_key(service));
    return true;
}

bool erase(std::string_view service) {
    if (service.empty()) return false;
    bool any = false;
    if (auth::keystore::available())
        any = auth::keystore::remove(keystore_key(service)) == auth::keystore::Status::Ok;
    auto all = read_all();
    if (all && all->erase(std::string{service}) > 0) any = write_all(*all) || any;
    return any;
}

} // namespace agentty::tools::web_search_secret
