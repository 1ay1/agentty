// agentty::auth::accounts — named per-provider credential slots.
//
// One JSON document, sealed at rest with crypt::seal (machine-bound). A
// legacy plaintext body is accepted and re-sealed on the next write.
//
// Every agentty on the machine shares this file, so each change is one
// read-modify-write inside its shared-file section (io/shared_file.hpp):
// two windows adding accounts at once both keep theirs.

#include "agentty/auth/accounts.hpp"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"          // config_dir()
#include "agentty/auth/cred_crypt.hpp"    // crypt::seal / unseal / looks_sealed
#include "agentty/io/shared_file.hpp"

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
    std::vector<Account>                            all;
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

Registry parse_registry(const std::optional<std::string>& file) {
    Registry reg;
    if (!file || file->empty()) return reg;
    std::string body;
    if (crypt::looks_sealed(*file)) {
        auto pt = crypt::unseal(*file);
        if (!pt) return reg;                // tampered / wrong machine
        body = std::move(*pt);
    } else {
        body = *file;                       // legacy plaintext
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
        // Not .items() on j.value(...) directly: value() returns a temporary
        // and items() would dangle (GCC 14 Release dropped the registry).
        const json active = j.value("active", json::object());
        for (const auto& [prov, lbl] : active.items())
            reg.set_active(prov, lbl.get<std::string>());
    } catch (...) {
        return Registry{};
    }
    return reg;
}

// Sealed bytes, or nullopt when sealing fails: secrets are never written plain.
std::optional<std::string> serialise(const Registry& reg) {
    json j;
    j["accounts"] = json::array();
    for (const auto& a : reg.all)
        j["accounts"].push_back({{"provider", a.provider}, {"label", a.label},
                                 {"secret", a.secret}, {"saved_at", a.saved_at_ms}});
    j["active"] = json::object();
    for (const auto& [p, l] : reg.active) j["active"][p] = l;
    return crypt::seal(j.dump(2));
}

Registry read_registry(Io io) { return parse_registry(persistence::read_file(io, registry_path())); }
Registry read_registry_in_section() { return parse_registry(persistence::peek_file(registry_path())); }

persistence::Lane& registry_lane() {
    static persistence::Lane l;
    return l;
}

// One change to the registry: read it inside the section, let `edit` change
// it (false = nothing to do), write it back. True only if it changed and
// was saved.
template <class F, class... Args>
bool mutate(const lock_levels::below_file_lane& cap, F edit, Args... args) {
    (void)edit;
    return persistence::with_shared_file(cap, registry_lane(), registry_path(),
        [](const maya::held<lock_levels::file_lane>&, persistence::Held, Args... a) {
            Registry reg = read_registry_in_section();
            if (!F{}(reg, std::move(a)...)) return false;
            const auto bytes = serialise(reg);
            if (!bytes) return false;
            return persistence::detail::write_in_section(registry_path().string(), *bytes,
                {.durability = persistence::Durability::Durable, .mode = 0600});
        }, std::move(args)...);
}

} // namespace

std::string path() { return registry_path().string(); }

std::vector<Account> list(Io io) {
    auto reg = read_registry(io);
    // Newest-saved first for a stable, useful default ordering in the picker.
    std::stable_sort(reg.all.begin(), reg.all.end(),
                     [](const Account& a, const Account& b) {
                         return a.saved_at_ms > b.saved_at_ms;
                     });
    return std::move(reg.all);
}

std::vector<Account> list_for(Io io, const std::string& provider) {
    std::vector<Account> out;
    for (auto& a : list(io))
        if (a.provider == provider) out.push_back(std::move(a));
    return out;
}

std::string active_label(Io io, const std::string& provider) {
    return read_registry(io).active_for(provider);
}

std::optional<Account> get(Io io, const std::string& provider, const std::string& label) {
    for (auto& a : read_registry(io).all)
        if (a.provider == provider && a.label == label) return a;
    return std::nullopt;
}

bool upsert(Io, const lock_levels::below_file_lane& cap, const std::string& provider,
            const std::string& label, const std::string& secret) {
    if (provider.empty() || label.empty()) return false;
    return mutate(cap, [](Registry& reg, std::string p, std::string l, std::string s) {
        const auto now = now_ms();
        bool found = false;
        for (auto& a : reg.all)
            if (a.provider == p && a.label == l) {
                a.secret = s; a.saved_at_ms = now; found = true; break;
            }
        if (!found) reg.all.push_back(Account{p, l, s, now});
        reg.set_active(p, l);
        return true;
    }, provider, label, secret);
}

bool set_active(Io, const lock_levels::below_file_lane& cap, const std::string& provider,
                const std::string& label) {
    return mutate(cap, [](Registry& reg, std::string p, std::string l) {
        const bool exists = std::ranges::any_of(reg.all, [&](const Account& a) {
            return a.provider == p && a.label == l;
        });
        if (exists) reg.set_active(p, l);
        return exists;
    }, provider, label);
}

bool remove(Io, const lock_levels::below_file_lane& cap, const std::string& provider,
            const std::string& label) {
    return mutate(cap, [](Registry& reg, std::string p, std::string l) {
        const auto before = reg.all.size();
        std::erase_if(reg.all, [&](const Account& a) { return a.provider == p && a.label == l; });
        if (reg.all.size() == before) return false;
        // Removed the active slot: promote the newest remaining one, if any.
        if (reg.active_for(p) == l) {
            std::string promote;
            std::int64_t best = -1;
            for (const auto& a : reg.all)
                if (a.provider == p && a.saved_at_ms > best) { best = a.saved_at_ms; promote = a.label; }
            if (!promote.empty()) reg.set_active(p, promote);
            else std::erase_if(reg.active, [&](const auto& kv) { return kv.first == p; });
        }
        return true;
    }, provider, label);
}

} // namespace agentty::auth::accounts
