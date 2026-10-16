// embed_secret.cpp — where the embeddings API key lives.
//
// Not in settings.json. A credential in a plaintext config file is a leak
// waiting for a screen-share, a dotfiles commit, or a bug-report attachment,
// and this one is pasted by users who are explicitly working around a locked
// -down environment — exactly the population that can least afford it.
//
// Two tiers, same shape as the rest of agentty's credential handling:
//   1. the OS keystore (libsecret / macOS Keychain) when it is available and
//      enabled, so the secret never touches agentty's own files;
//   2. otherwise a sealed file under the config dir (auth::crypt::seal —
//      machine-bound AEAD), which is what credentials.json already uses.
//
// Keyed by ENDPOINT, not by "the embeddings key": a user may point different
// projects at different gateways, and the key that authenticates one must not
// leak into a request to another.

#include "agentty/rag/embed_secret.hpp"

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/auth/keystore.hpp"
#include "agentty/io/shared_file.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace agentty::rag::embed {

namespace {

namespace fs = std::filesystem;

// One file for every endpoint's key, sealed as a unit.
[[nodiscard]] fs::path secrets_path() {
    return auth::config_dir() / "embed_keys.json";
}

[[nodiscard]] std::string keystore_key(const std::string& endpoint) {
    return "agentty:embed:" + endpoint;
}

[[nodiscard]] nlohmann::json parse_all(const std::optional<std::string>& file) {
    if (!file || file->empty()) return nlohmann::json::object();
    const std::string& raw = *file;
    // Sealed envelope, or (legacy/hand-written) plaintext JSON.
    if (auto opened = auth::crypt::unseal(raw)) {
        try { return nlohmann::json::parse(*opened); } catch (...) { }
        return nlohmann::json::object();
    }
    try {
        auto j = nlohmann::json::parse(raw);
        if (j.is_object()) return j;
    } catch (...) { }
    return nlohmann::json::object();
}

[[nodiscard]] nlohmann::json read_all() { return parse_all(persistence::peek_file(secrets_path())); }

// Set (or with an empty key, drop) one endpoint's key: the whole file is read,
// changed and rewritten in its shared-file section, so two agentty windows
// saving different endpoints both keep theirs. Sealed, 0600, atomic; an
// unsealable secret is refused rather than written plain. True when the file
// now holds what was asked (dropping an absent key is a no-op success).
bool update_key(Io io, const lock_levels::below_file_lane& cap, const std::string& endpoint,
                const std::string& key) {
    // Sealed inside the section (the body depends on what's on disk now). An
    // unsealable result writes nothing: a secret is never stored plain.
    const auto r = persistence::update_shared(io, cap, secrets_path(),
        {.durability = persistence::Durability::Durable, .mode = 0600},
        [](std::optional<std::string> cur, std::string ep, std::string k)
            -> std::optional<std::string> {
            auto all = parse_all(cur);
            if (k.empty()) { if (all.erase(ep) == 0) return std::nullopt; }
            else all[ep] = k;
            return auth::crypt::seal(all.dump());
        }, endpoint, key);
    if (r == persistence::Update::Written) return true;
    // Unchanged is success only for dropping a key that wasn't there; for a
    // set it means sealing failed.
    return r == persistence::Update::Unchanged && key.empty();
}

} // namespace

std::string load_key(const std::string& endpoint) {
    if (endpoint.empty()) return {};
    if (auth::keystore::available()) {
        std::string out;
        if (auth::keystore::retrieve(keystore_key(endpoint), out) == auth::keystore::Status::Ok)
            return out;
    }
    const auto all = read_all();
    if (auto it = all.find(endpoint); it != all.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

bool store_key(Io io, const lock_levels::below_file_lane& cap, const std::string& endpoint,
               const std::string& key) {
    if (endpoint.empty()) return false;
    if (key.empty()) return erase_key(io, cap, endpoint);

    if (auth::keystore::available()
        && auth::keystore::store(keystore_key(endpoint), key) == auth::keystore::Status::Ok) {
        // Belt and braces: drop any stale sealed copy so there is exactly one
        // home for this secret.
        (void)update_key(io, cap, endpoint, {});
        return true;
    }
    return update_key(io, cap, endpoint, key);
}

bool erase_key(Io io, const lock_levels::below_file_lane& cap, const std::string& endpoint) {
    if (endpoint.empty()) return false;
    bool any = false;
    if (auth::keystore::available())
        any = auth::keystore::remove(keystore_key(endpoint)) == auth::keystore::Status::Ok;
    const bool had = read_all().contains(endpoint);
    if (had) any = update_key(io, cap, endpoint, {}) || any;
    return any;
}

std::string endpoint_key(const EmbedConfig& c) {
    if (!needs_api_key(c.backend)) return {};
    std::string out;
    out += id_of(c.backend);
    out += "://";
    out += c.host;
    out += ':';
    out += std::to_string(c.port);
    if (!c.path.empty()) out += c.path;
    return out;
}

} // namespace agentty::rag::embed
