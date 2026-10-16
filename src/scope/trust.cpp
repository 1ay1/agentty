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

#include "agentty/auth/auth.hpp"        // auth::sha256_hex
#include "agentty/io/shared_file.hpp"

#include <algorithm>
#include <cstdlib>
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
namespace {
std::vector<std::string> parse_list(const std::optional<std::string>& bytes) {
    std::vector<std::string> out;
    if (!bytes) return out;
    try {
        auto doc = nlohmann::json::parse(*bytes, nullptr, /*throw=*/false);
        if (doc.is_array())
            for (const auto& v : doc)
                if (v.is_string()) out.emplace_back(v.get<std::string>());
    } catch (...) { out.clear(); }
    return out;
}

void merge_into(std::vector<std::string>& into, std::vector<std::string> add) {
    for (auto& s : add)
        if (std::ranges::find(into, s) == into.end()) into.push_back(std::move(s));
}
}  // namespace

// Where the store lives, and where it used to (the flat user root). Empty
// paths when there's no safe home.
struct ApprovalPaths { fs::path dest, legacy; };

[[nodiscard]] ApprovalPaths approvals_paths(std::string_view leaf) {
    const char* override = std::getenv("AGENTTY_HOME");
    // Fail closed with no home and no override: util::user_root() would
    // otherwise fall back to $PWD/.agentty and drop approvals into whatever
    // directory agentty was launched from — which could be a project, the
    // exact repo-vouches-for-itself hazard this store exists to stop.
    if (util::home_dir_or_empty().empty() && !(override && *override))
        return {};
    const fs::path root = util::user_root();
    if (root.empty()) return {};

    // state/ (or $AGENTTY_STATE_DIR): machine-written, never hand-edited.
    const fs::path dir = util::user_state_dir();
    if (dir.empty()) return {};
    return {dir / leaf, root / leaf};
}
}  // namespace

Approvals load_approvals(std::string_view leaf) noexcept {
    // Lock-free: callers include lock bodies (an MCP connect round). The old
    // flat file is read too, so approvals there still count until the next
    // save folds them in.
    Approvals a;
    const auto [dest, legacy] = approvals_paths(leaf);
    if (dest.empty()) return a;
    // Fail CLOSED: any parse trouble → empty approvals → project config stays
    // Pending. Never throw out of this noexcept trust path.
    try {
        a.shas = parse_list(persistence::peek_file(dest));
        merge_into(a.shas, parse_list(persistence::peek_file(legacy)));
    } catch (...) { a.shas.clear(); }
    return a;
}

bool save_approvals(Io io, const lock_levels::below_file_lane& cap, std::string_view leaf,
                    const Approvals& a) noexcept {
    const auto [dest, legacy] = approvals_paths(leaf);
    if (dest.empty()) return false;
    // Re-read + merge + write in one section. Approvals are a grow-only set
    // of content hashes (nothing revokes one), so union is the right merge:
    // two processes approving different things at once both keep theirs.
    // The old flat file is folded in and removed here, under the same lock.
    try {
        return persistence::Update::Failed != persistence::update_shared(io, cap, dest,
            {.durability = persistence::Durability::Durable},
            [](std::optional<std::string> cur, std::vector<std::string> add, std::string old)
                -> std::optional<std::string> {
                auto merged = parse_list(cur);
                merge_into(merged, std::move(add));
                merge_into(merged, parse_list(persistence::peek_file(old)));
                std::error_code rm;
                fs::remove(old, rm);
                return nlohmann::json(merged).dump(2);
            }, a.shas, legacy.string());
    } catch (...) { return false; }
}

}  // namespace agentty::scope
