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

#include "agentty/auth/auth.hpp"   // auth::sha256_hex, CrossProcessFileLock

#ifndef _WIN32
#include <unistd.h>   // getpid
#else
#include <process.h>  // _getpid
#define getpid _getpid
#endif

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

    // state/: machine-written, accumulated, never hand-edited -- the same
    // category the project root uses for its feedback TSV. Keeps the user
    // root from growing one loose *_approved.json per subsystem.
    std::error_code ec;
    const fs::path dir = root / "state";
    fs::create_directories(dir, ec);
    const fs::path dest = dir / leaf;

    // Adopt an approval list from the old flat location. Losing one means a
    // skill or hook the user already vouched for silently reverts to
    // pending, so this moves rather than orphans.
    //
    // The lock matters as much as the move. save_approvals locks the path
    // this RETURNS, so a rename done here would sit outside it: two
    // processes could both see the legacy file, both rename, and the loser
    // clobbers the winner's merged list. Lock the destination first, then
    // re-check -- whoever gets the lock second finds dest already there and
    // does nothing.
    const fs::path legacy = root / leaf;
    if (fs::is_regular_file(legacy, ec) && !fs::exists(dest, ec)) {
        auth::CrossProcessFileLock guard{dest};
        if (!fs::exists(dest, ec)) {
            std::error_code rec;
            fs::rename(legacy, dest, rec);
        }
    }
    return dest;
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

    // Hold the file lock across the whole re-read + merge + write.
    //
    // Every caller here does load_approvals -> approve(x) -> save_approvals,
    // which is a load->modify->store cycle on a file three independent
    // subsystems share (skills, hooks, MCP plugins). Two agentty processes
    // approving different things at once each wrote a list that had never
    // seen the other's entry, so one approval silently vanished and the
    // thing it vouched for went back to Pending. Measured before the fix:
    // 4 concurrent approvals, 10 rounds, only 1 survived every time.
    //
    // Best-effort, like every other use of this lock: if it cannot be taken
    // we still do the merge below, which is strictly better than the
    // truncating write this replaced.
    auth::CrossProcessFileLock guard{p};

    try {
        // MERGE rather than replace. Approvals are a grow-only set of
        // content hashes -- an entry means "a human vouched for these exact
        // bytes" and nothing in the tree revokes one -- so union is the
        // correct reconciliation, not last-writer-wins. That also makes the
        // write idempotent under a lost lock.
        std::vector<std::string> merged;
        {
            std::ifstream in(p);
            if (in) {
                nlohmann::json cur =
                    nlohmann::json::parse(in, nullptr, /*throw=*/false);
                if (cur.is_array())
                    for (const auto& v : cur)
                        if (v.is_string()) merged.push_back(v.get<std::string>());
            }
        }
        for (const auto& s : a.shas)
            if (std::find(merged.begin(), merged.end(), s) == merged.end())
                merged.push_back(s);

        nlohmann::json doc = nlohmann::json::array();
        for (const auto& s : merged) doc.push_back(s);

        // Write through a unique temp + atomic rename so a reader never sees
        // a half-written list, and a crash mid-write cannot truncate the
        // store to nothing. The old form opened the real path with trunc,
        // which meant the file was momentarily EMPTY on every save -- and an
        // empty approvals file fails closed, so a concurrent reader could
        // see every project config as untrusted.
        const fs::path tmp = p.parent_path()
                           / (p.filename().string() + "."
                              + std::to_string(static_cast<long long>(getpid()))
                              + ".tmp");
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out) return false;
            out << doc.dump(2);
            if (!out) { fs::remove(tmp, ec); return false; }
        }
        fs::rename(tmp, p, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    } catch (...) { return false; }
}

}  // namespace agentty::scope
