// trust.cpp — the persistence + hashing half of scope's trust store.
//
// Split from scope.cpp so the pure algebra there pulls in neither auth
// (SHA-256) nor nlohmann/json — the lean standalone scope/skills/commands
// tests link only scope.cpp. This TU is pulled by the full binary and the
// dedicated trust test.
//
// The store lives under the USER root (util::user_root(): ~/.agentty, or
// $AGENTTY_HOME when overridden — see approvals_path below), keyed by a
// content hash, so a cloned repo can neither write an approval for itself
// nor keep one valid after its command bytes change (the MCPoison /
// CVE-2025-54136 re-gate).

#include "agentty/scope/scope.hpp"
#include "agentty/util/home_dir.hpp"
#include "agentty/util/user_root.hpp"

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
// The approvals file lives under the USER root — util::user_root()
// (~/.agentty, or $AGENTTY_HOME when overridden) — never the project's, so a
// cloned repo can't write an approval for itself. Resolving through
// user_root() rather than joining $HOME + dir_name(Dialect::Agentty) is what
// makes trust FOLLOW the relocated store: with AGENTTY_HOME set, hooks, MCP
// and skills approvals land beside the credentials they gate instead of
// staying behind in the real ~/.agentty, where they were lost on every
// relocated run (each scratch run re-prompted and re-approved into /tmp).
// The default resolves identically: user_root() is $HOME/.agentty. Empty
// (fail-closed) when there is neither a real home nor $AGENTTY_HOME.
[[nodiscard]] fs::path approvals_path(std::string_view leaf) {
    const char* override = std::getenv("AGENTTY_HOME");
    // Fail closed with no home and no override: util::user_root() would
    // otherwise fall back to $PWD/.agentty and drop approvals into whatever
    // directory agentty was launched from — which could be a project, the
    // exact repo-vouches-for-itself hazard this store exists to stop.
    if (util::home_dir_or_empty().empty() && !(override && *override))
        return {};
    const fs::path root = util::user_root();
    if (root.empty()) return {};
    // No `dir_name` join: user_root() already IS the .agentty dir.
    return root / leaf;
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
