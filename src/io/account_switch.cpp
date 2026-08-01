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
#include <iterator>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/auth/cred_crypt.hpp"
#include "agentty/provider/chatgpt/codex_oauth.hpp"

namespace agentty::auth::accounts {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace crypt = agentty::auth::crypt;
namespace chatgpt = agentty::provider::chatgpt;

// The active-store file for a provider whose credential is a single file.
// nullopt for providers whose accounts are handled elsewhere (OpenAI keys
// already switch per-endpoint through Settings.provider_keys).
std::optional<fs::path> active_store_file(const std::string& provider) {
    if (provider == "anthropic" || provider.empty())
        return agentty::auth::credentials_path();
    if (provider == "chatgpt")
        return chatgpt::codex_credentials_path();
    return std::nullopt;
}

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

} // namespace

bool snapshot_active(const std::string& provider, const std::string& label) {
    auto file = active_store_file(provider);
    if (!file) return false;
    auto blob = read_all(*file);
    if (!blob) return false;                 // nothing signed in to capture
    return upsert(provider, label, *blob);
}

bool activate(const std::string& provider, const std::string& label) {
    auto slot = get(provider, label);
    if (!slot) return false;
    auto file = active_store_file(provider);
    if (!file) return false;
    if (!write_store(*file, slot->secret)) return false;
    // Mirror it into the OS keystore when that's the primary store, so the
    // next resolve() reads the switched-to account and not a stale keychain
    // entry. save_credentials/save_codex handle the keystore on their own
    // paths, but here we wrote the file directly — re-run the provider's
    // loader→saver round-trip to re-seal into the keystore.
    if (provider == "anthropic" || provider.empty()) {
        if (auto c = agentty::auth::load_credentials())
            agentty::auth::save_credentials(*c);
    } else if (provider == "chatgpt") {
        if (auto c = chatgpt::load_codex_credentials())
            chatgpt::save_codex_credentials(*c);
    }
    return set_active(provider, label);
}

std::string derive_current_label(const std::string& provider) {
    auto file = active_store_file(provider);
    if (!file) return {};
    auto blob = read_all(*file);
    if (!blob) return {};
    auto body = body_of(*blob);
    if (!body) return "signed in";           // sealed for another machine

    // Try to pull a human-recognisable identifier out of the credential.
    // We deliberately avoid JWT-decoding the id_token (no base64url decoder
    // in the crypt layer, and the payload is unverified) — the account_id
    // and method fields are plain JSON and enough to disambiguate slots.
    try {
        json j = json::parse(*body);
        if (provider == "chatgpt") {
            std::string acct = j.value("account_id", "");
            if (!acct.empty()) return "ChatGPT " + acct.substr(0, 8);
            return "ChatGPT";
        }
        std::string method = j.value("method", "");
        if (method == "api_key" || method == "apikey") return "API key";
        if (method == "oauth")   return "OAuth login";
        if (!method.empty())     return method;
    } catch (...) {}
    return "signed in";
}

} // namespace agentty::auth::accounts
