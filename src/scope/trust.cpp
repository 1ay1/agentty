// trust.cpp — the persistence + hashing half of scope's trust store.
//
// Split from scope.cpp so the pure algebra there pulls in neither auth
// (SHA-256) nor nlohmann/json — the lean standalone scope/skills/commands
// tests link only scope.cpp. This TU is pulled by the full binary and the
// dedicated trust test.
//
// The store lives under the USER .agentty dir, keyed by a content hash, so a
// cloned repo can neither write an approval for itself nor keep one valid
// after its command bytes change (the MCPoison / CVE-2025-54136 re-gate).

#include "agentty/scope/scope.hpp"

#include "agentty/auth/auth.hpp"   // auth::sha256_hex

#include <cstdlib>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace agentty::scope {

std::string content_hash(std::string_view bytes) noexcept {
    // Reuse the auth SHA-256 (the same primitive hooks' approval store uses),
    // so trust identity is consistent across the two content-hash gates.
    return agentty::auth::sha256_hex(std::string{bytes});
}

namespace {
// Renamed from home_dir() to keep internal linkage unique under a unity build
// (scope.cpp defines an identical anonymous-namespace home_dir(); concatenated
// into one TU they'd collide). Same behaviour, file-local name.
[[nodiscard]] fs::path trust_home_dir() noexcept {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return fs::path{u};
#endif
    return {};
}

// The approvals file lives under the USER .agentty dir — never the project's,
// so a cloned repo can't write an approval for itself. Empty if no HOME.
[[nodiscard]] fs::path approvals_path(std::string_view leaf) {
    const fs::path h = trust_home_dir();
    if (h.empty()) return {};
    return h / dir_name(Dialect::Agentty) / leaf;
}
}  // namespace

Approvals load_approvals(std::string_view leaf) noexcept {
    Approvals a;
    const fs::path p = approvals_path(leaf);
    if (p.empty()) return a;
    std::ifstream in(p);
    if (!in) return a;
    // Fail CLOSED: any parse trouble → empty approvals → project config stays
    // Pending. Never throw out of this noexcept trust path.
    try {
        nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*throw=*/false);
        if (doc.is_array())
            for (const auto& v : doc)
                if (v.is_string()) a.shas.emplace_back(v.get<std::string>());
    } catch (...) { a.shas.clear(); }
    return a;
}

bool save_approvals(std::string_view leaf, const Approvals& a) noexcept {
    const fs::path p = approvals_path(leaf);
    if (p.empty()) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    try {
        nlohmann::json doc = nlohmann::json::array();
        for (const auto& s : a.shas) doc.push_back(s);
        std::ofstream out(p, std::ios::trunc);
        if (!out) return false;
        out << doc.dump(2);
        return static_cast<bool>(out);
    } catch (...) { return false; }
}

}  // namespace agentty::scope
