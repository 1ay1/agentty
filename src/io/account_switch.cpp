// agentty::auth::accounts — provider-specific adapters that bridge the
// opaque account registry to each provider's ACTIVE credential store.
//
// Strategy: snapshot/restore the raw on-disk credential BLOB for the active
// store, byte-for-byte. This is provider-agnostic (works whether the file is
// sealed or legacy-plaintext) and preserves perfect fidelity — a restored
// account is indistinguishable from having just logged into it. The registry
// itself is separately sealed, so the plaintext blob never rests on disk
// outside the provider's own (already 0600, keystore-backed) store.

#include "agentty/auth/accounts.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <iterator>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/io/persistence.hpp"       // load/save_settings (custom hosts)
#include "agentty/provider/registry.hpp"      // preset_for (custom-host detect)
#include "agentty/provider/chatgpt/codex_oauth.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/util/dbglog.hpp"

namespace agentty::auth::accounts {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace crypt = agentty::auth::crypt;
namespace chatgpt = agentty::provider::chatgpt;
namespace copilot = agentty::provider::copilot;
namespace kimi = agentty::provider::kimi;

// Read a file whole; nullopt when missing/empty.
std::optional<std::string> read_all(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(ifs)),
                  std::istreambuf_iterator<char>());
    if (s.empty()) return std::nullopt;
    return s;
}

// Write a blob to a store file atomically, 0600. Returns success.
bool write_store(const fs::path& p, const std::string& blob) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!ofs) return false;
    }
#ifndef _WIN32
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
#endif
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// Decrypt a store blob to its JSON body (accepts sealed or plaintext).
std::optional<std::string> body_of(const std::string& blob) {
    if (crypt::looks_sealed(blob)) return crypt::unseal(blob);
    return blob;
}

// A CUSTOM HOST's account provider id IS its endpoint spec (the key under
// which its bearer key lives in Settings.provider_keys). Custom hosts have no
// credential FILE — their "active credential" is that provider_keys[spec]
// entry — so the four file-backed providers (and empty→anthropic) are NOT
// custom hosts; everything else non-empty is treated as a spec.
bool is_custom_host_provider(const std::string& p) {
    return !p.empty() && p != "anthropic" && p != "chatgpt"
        && p != "copilot" && p != "kimi";
}

// ── One backend per provider ────────────────────────────────────────────────
// The ONLY thing that differs between providers is (a) WHERE the active
// credential lives — a file, or Settings.provider_keys[spec] — and (b) how to
// re-seal it into a keystore + label it. Capture that once so snapshot_active /
// activate / derive_current_label share a single, uniform body.
enum class Store : std::uint8_t { File, SettingsKey };

struct Backend {
    Store           store;
    fs::path        file;                 // when store == File
    std::function<std::string(const json&)> label_body;   // File: label from JSON body
    std::function<void()> after_activate;  // File: keystore re-seal / cache bust
};

// Read the SettingsKey (custom-host) secret, or empty.
std::string settings_key(const std::string& provider) {
    auto s = agentty::persistence::load_settings();
    auto it = s.provider_keys.find(provider);
    return it != s.provider_keys.end() ? it->second : std::string{};
}
void set_settings_key(const std::string& provider, const std::string& v) {
    auto s = agentty::persistence::load_settings();
    s.provider_keys[provider] = v;
    agentty::persistence::save_settings(s);
}

// Resolve the backend for a provider id.
Backend backend_for(const std::string& provider) {
    Backend b;
    if (is_custom_host_provider(provider)) {
        b.store = Store::SettingsKey;
        return b;
    }
    b.store = Store::File;
    if (provider == "anthropic" || provider.empty()) {
        b.file = agentty::auth::credentials_path();
        b.label_body = [](const json& j) -> std::string {
            std::string method = j.value("method", "");
            if (method == "api_key" || method == "apikey") return "API key";
            if (method == "oauth")   return "OAuth login";
            if (!method.empty())     return method;
            return "signed in";
        };
        b.after_activate = [] {
            if (auto c = agentty::auth::load_credentials())
                agentty::auth::save_credentials(*c);   // re-seal to keystore
        };
    } else if (provider == "chatgpt") {
        b.file = chatgpt::codex_credentials_path();
        b.label_body = [](const json& j) -> std::string {
            std::string acct = j.value("account_id", "");
            return acct.empty() ? "ChatGPT" : "ChatGPT " + acct.substr(0, 8);
        };
        b.after_activate = [] {
            if (auto c = chatgpt::load_codex_credentials())
                chatgpt::save_codex_credentials(*c);
        };
    } else if (provider == "copilot") {
        b.file = copilot::credentials_path();
        b.label_body = [](const json& j) -> std::string {
            std::string sku;
            if (j.contains("proxy") && j["proxy"].is_object())
                sku = j["proxy"].value("sku", "");
            std::string gh = j.value("github_token", "");
            std::string tag = gh.size() >= 8 ? gh.substr(gh.size() - 4) : "";
            std::string base = sku.empty() ? "Copilot" : "Copilot (" + sku + ")";
            return tag.empty() ? base : base + " \xe2\x80\xa6" + tag;
        };
        b.after_activate = [] { copilot::invalidate_cached_token(); };
    } else if (provider == "kimi") {
        b.file = kimi::credentials_path();
        b.label_body = [](const json& j) -> std::string {
            std::string rt = j.value("refresh_token", std::string{});
            if (rt.empty()) rt = j.value("access_token", std::string{});
            std::string tag = rt.size() >= 4 ? rt.substr(rt.size() - 4) : "";
            return tag.empty() ? std::string{"Kimi"} : "Kimi \xe2\x80\xa6" + tag;
        };
        b.after_activate = [] { kimi::invalidate_cached_token(); };
    }
    return b;
}

// A stable, human-recognisable label for a SettingsKey (custom-host) bearer
// key — prefix…suffix isn't unique enough, so a 6-hex hash of the WHOLE key
// guarantees distinct keys get distinct labels.
std::string settings_key_label(const std::string& key) {
    if (key.empty()) return {};
    std::string tail = key.size() >= 4 ? key.substr(key.size() - 4) : key;
    const std::uint32_t h = std::hash<std::string>{}(key) & 0xffffffu;
    char hx[7];
    std::snprintf(hx, sizeof(hx), "%06x", h);
    return "key \xe2\x80\xa6" + tail + " #" + std::string(hx, 6);
}

} // namespace

bool snapshot_active(const std::string& provider, const std::string& label) {
    const Backend b = backend_for(provider);
    if (b.store == Store::SettingsKey) {
        const std::string key = settings_key(provider);
        if (key.empty()) return false;
        return upsert(provider, label, key);
    }
    auto blob = read_all(b.file);
    if (!blob) return false;                 // nothing signed in to capture
    return upsert(provider, label, *blob);
}

bool activate(const std::string& provider, const std::string& label) {
    auto slot = get(provider, label);
    if (!slot) return false;
    const Backend b = backend_for(provider);
    if (b.store == Store::SettingsKey) {
        set_settings_key(provider, slot->secret);   // the endpoint resolve() reads
        return set_active(provider, label);
    }
    if (!write_store(b.file, slot->secret)) return false;
    // Re-seal into the keystore / bust cached tokens so the next resolve()
    // reads the switched-to account, not a stale entry.
    if (b.after_activate) b.after_activate();
    return set_active(provider, label);
}

std::string derive_current_label(const std::string& provider) {
    const Backend b = backend_for(provider);
    if (b.store == Store::SettingsKey)
        return settings_key_label(settings_key(provider));
    auto blob = read_all(b.file);
    if (!blob) return {};
    auto body = body_of(*blob);
    if (!body) return "signed in";           // sealed for another machine
    try {
        json j = json::parse(*body);
        if (b.label_body) return b.label_body(j);
    } catch (const std::exception& e) {
        util::dbglog("accounts.derive_label.parse", e.what());
    } catch (...) {
        util::dbglog("accounts.derive_label.parse", "non-std exception");
    }
    return "signed in";
}

} // namespace agentty::auth::accounts
