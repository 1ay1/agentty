// agentty::auth::accounts — named per-provider credential slots.
//
// Persistence mirrors credentials.json exactly: a single JSON document,
// sealed at rest with crypt::seal (machine-bound) when possible, written
// atomically. A legacy plaintext body is accepted and re-sealed on the next
// write, so no migration step is needed.

#include "agentty/auth/accounts.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"          // config_dir()
#include "agentty/auth/cred_crypt.hpp"    // crypt::seal / unseal / looks_sealed

namespace agentty::auth::accounts {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace crypt = agentty::auth::crypt;

fs::path registry_path() { return agentty::auth::config_dir() / "accounts.json"; }

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// In-memory shape: provider → {active_label, accounts[]}.
struct Registry {
    // Preserve insertion/save order; newest upsert bubbles to front on read.
    std::vector<Account>                          all;
    std::vector<std::pair<std::string,std::string>> active;  // provider → label

    [[nodiscard]] std::string active_for(const std::string& provider) const {
        for (const auto& [p, l] : active) if (p == provider) return l;
        return {};
    }
    void set_active(const std::string& provider, const std::string& label) {
        for (auto& [p, l] : active) if (p == provider) { l = label; return; }
        active.emplace_back(provider, label);
    }
};

Registry read_registry() {
    Registry reg;
    std::string raw;
    {
        std::ifstream ifs(registry_path(), std::ios::binary);
        if (!ifs) return reg;
        raw.assign((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());
    }
    if (raw.empty()) return reg;

    std::string body;
    if (crypt::looks_sealed(raw)) {
        auto pt = crypt::unseal(raw);
        if (!pt) return reg;                // tampered / wrong machine
        body = std::move(*pt);
    } else {
        body = std::move(raw);              // legacy plaintext
    }

    try {
        json j = json::parse(body);
        for (const auto& a : j.value("accounts", json::array())) {
            Account acc;
            acc.provider    = a.value("provider", "");
            acc.label       = a.value("label", "");
            acc.secret      = a.value("secret", "");
            acc.saved_at_ms = a.value("saved_at", std::int64_t{0});
            if (!acc.provider.empty() && !acc.label.empty())
                reg.all.push_back(std::move(acc));
        }
        for (const auto& [prov, lbl] : j.value("active", json::object()).items())
            reg.set_active(prov, lbl.get<std::string>());
    } catch (...) {
        return Registry{};
    }
    return reg;
}

bool write_registry(const Registry& reg) {
    json j;
    j["accounts"] = json::array();
    for (const auto& a : reg.all) {
        j["accounts"].push_back({
            {"provider", a.provider},
            {"label",    a.label},
            {"secret",   a.secret},
            {"saved_at", a.saved_at_ms},
        });
    }
    j["active"] = json::object();
    for (const auto& [p, l] : reg.active) j["active"][p] = l;

    std::string payload = j.dump(2);
    // Encrypt at rest; refuse to persist plaintext secrets if sealing fails.
    auto sealed = crypt::seal(payload);
    if (!sealed) return false;

    // Best-effort atomic-ish write via the auth layer's private writer would
    // be ideal, but it isn't exported; a truncating write is acceptable here
    // because the registry is machine-local metadata (secrets are also in the
    // active stores) and corruption degrades to "no saved accounts", not a
    // security issue.
    fs::path p = registry_path();
    fs::path tmp = p; tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(sealed->data(), static_cast<std::streamsize>(sealed->size()));
        if (!ofs) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

} // namespace

std::string path() { return registry_path().string(); }

std::vector<Account> list() {
    auto reg = read_registry();
    // Newest-saved first for a stable, useful default ordering in the picker.
    std::stable_sort(reg.all.begin(), reg.all.end(),
                     [](const Account& a, const Account& b) {
                         return a.saved_at_ms > b.saved_at_ms;
                     });
    return std::move(reg.all);
}

std::vector<Account> list_for(const std::string& provider) {
    std::vector<Account> out;
    for (auto& a : list())
        if (a.provider == provider) out.push_back(std::move(a));
    return out;
}

std::string active_label(const std::string& provider) {
    return read_registry().active_for(provider);
}

std::optional<Account> get(const std::string& provider, const std::string& label) {
    for (auto& a : read_registry().all)
        if (a.provider == provider && a.label == label) return a;
    return std::nullopt;
}

bool upsert(const std::string& provider, const std::string& label,
            const std::string& secret) {
    if (provider.empty() || label.empty()) return false;
    auto reg = read_registry();
    bool found = false;
    for (auto& a : reg.all) {
        if (a.provider == provider && a.label == label) {
            a.secret      = secret;
            a.saved_at_ms = now_ms();
            found = true;
            break;
        }
    }
    if (!found) {
        Account a;
        a.provider    = provider;
        a.label       = label;
        a.secret      = secret;
        a.saved_at_ms = now_ms();
        reg.all.push_back(std::move(a));
    }
    reg.set_active(provider, label);
    return write_registry(reg);
}

bool set_active(const std::string& provider, const std::string& label) {
    auto reg = read_registry();
    bool exists = false;
    for (const auto& a : reg.all)
        if (a.provider == provider && a.label == label) { exists = true; break; }
    if (!exists) return false;
    reg.set_active(provider, label);
    return write_registry(reg);
}

bool remove(const std::string& provider, const std::string& label) {
    auto reg = read_registry();
    const auto before = reg.all.size();
    std::erase_if(reg.all, [&](const Account& a) {
        return a.provider == provider && a.label == label;
    });
    if (reg.all.size() == before) return false;

    // If we removed the active slot, promote the newest remaining account
    // for that provider (if any) to active; otherwise clear the entry.
    if (reg.active_for(provider) == label) {
        std::string promote;
        std::int64_t best = -1;
        for (const auto& a : reg.all)
            if (a.provider == provider && a.saved_at_ms > best) {
                best = a.saved_at_ms; promote = a.label;
            }
        if (!promote.empty()) reg.set_active(provider, promote);
        else std::erase_if(reg.active,
                           [&](const auto& kv) { return kv.first == provider; });
    }
    return write_registry(reg);
}

} // namespace agentty::auth::accounts
