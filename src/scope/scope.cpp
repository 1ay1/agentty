// scope.cpp — the impure edge of agentty::scope.
//
// Everything that touches the filesystem or the environment lives HERE; the
// header stays a pure, testable algebra (the two folds are inline templates
// there). Three things need a translation unit:
//   • plan()        — turn (Layout, Env) into the ordered Source list.
//   • current_env() — read HOME / project_root / writability once, at the edge.
//   • trust_of() + Approvals — the content-hash trust store (the MCPoison fix).

#include "agentty/scope/scope.hpp"

#include "agentty/tool/util/fs_helpers.hpp"   // util::project_root()

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>

#if !defined(_WIN32)
#  include <unistd.h>   // access(), W_OK
#endif

namespace agentty::scope {

namespace {

// The three dialect dirs in native-first precedence order — the within-locus
// tiebreak. Kept in one place so plan() reads as the Locus × Dialect product.
constexpr Dialect kDialects[] = {Dialect::Agentty, Dialect::Agents, Dialect::Claude};

// Is `dir` a real directory we can create/write files in? Generalises the
// probe memory does before offering Project scope. Non-throwing.
[[nodiscard]] bool dir_writable(const fs::path& dir) noexcept {
    if (dir.empty()) return false;
    std::error_code ec;
    // Walk up to the nearest existing ancestor — writing "<root>/.agentty/x"
    // only needs <root> (or its first existing parent) to be writable.
    fs::path probe = dir;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        auto parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    if (probe.empty() || !fs::is_directory(probe, ec)) return false;
#if defined(_WIN32)
    return true;  // no cheap W_OK; the write itself surfaces failure via Io
#else
    return ::access(probe.c_str(), W_OK) == 0;
#endif
}

[[nodiscard]] fs::path home_dir() noexcept {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return fs::path{u};
#endif
    return {};
}

// A Project/Local root only exists if project_root() is a real, non-root dir.
// "/" is agentty's "unrestricted access boundary" sentinel, never a place to
// scatter .agentty state — matching memory's guard exactly.
[[nodiscard]] bool usable_project_root(const fs::path& p) noexcept {
    if (p.empty()) return false;
    return p != p.root_path();   // reject "/" (and "C:\") as a project root
}

}  // namespace

std::vector<Source> plan(const Layout& layout, const Env& env) {
    std::vector<Source> out;
    out.reserve(1 + 2 * std::size(kDialects));

    // Explicit — an env-pointed file the user aimed at us. Highest precedence,
    // dialect-agnostic (the user named the exact path). Its base is the file's
    // PARENT so Source::base stays "the dir this config lives in".
    if (env.explicit_config && !env.explicit_config->empty()) {
        Source s;
        s.locus    = Locus::Explicit;
        s.dialect  = Dialect::Agentty;         // convention; the leaf is explicit
        s.base     = env.explicit_config->parent_path();
        s.writable = dir_writable(s.base);
        out.push_back(std::move(s));
    }

    // Project — committed, shared. One Source per dialect dir, native first.
    // (Locus::Local is deliberately NOT emitted yet — the value exists in the
    // lattice, but no consumer opts in. See the header's migration note.)
    if (usable_project_root(env.project_root)) {
        for (Dialect d : kDialects) {
            Source s;
            s.locus    = Locus::Project;
            s.dialect  = d;
            s.base     = env.project_root / dir_name(d);
            s.writable = env.project_writable && d == Dialect::Agentty;
            out.push_back(std::move(s));
        }
    }

    // User — global, across every workspace. One Source per dialect dir.
    if (!env.home.empty()) {
        for (Dialect d : kDialects) {
            Source s;
            s.locus    = Locus::User;
            s.dialect  = d;
            s.base     = env.home / dir_name(d);
            s.writable = (d == Dialect::Agentty) && dir_writable(env.home);
            out.push_back(std::move(s));
        }
    }

    (void)layout;  // plan lays out ROOTS; the leaf is joined by the caller/loader
    return out;
}

Env current_env(const Layout& layout) {
    Env e;
    e.home         = home_dir();
    e.project_root = tools::util::project_root();
    e.project_writable =
        usable_project_root(e.project_root) &&
        dir_writable(e.project_root / dir_name(Dialect::Agentty));

    if (!layout.explicit_env.empty()) {
        if (const char* v = std::getenv(std::string{layout.explicit_env}.c_str());
            v && *v) {
            std::error_code ec;
            fs::path p{v};
            if (fs::is_regular_file(p, ec)) e.explicit_config = std::move(p);
        }
    }
    return e;
}

// ── Trust ─────────────────────────────────────────────────────────────────

bool Approvals::approved(std::string_view content_sha) const noexcept {
    return std::find(shas.begin(), shas.end(), content_sha) != shas.end();
}

void Approvals::approve(std::string_view content_sha) {
    if (!approved(content_sha)) shas.emplace_back(content_sha);
}

// content_hash / load_approvals / save_approvals live in src/scope/trust.cpp
// — they depend on auth (SHA-256) and nlohmann/json, which the lean scope
// core (and the standalone scope/skills/commands tests that link only this
// TU) must not pull in. trust_of() below stays here: it's pure.


Trust trust_of(const Source& src,
               std::string_view content_sha,
               const Approvals& approvals) noexcept {
    // Explicit / User: the human placed this file themselves — implicitly
    // trusted. (Both are outside a cloned repo's reach.)
    if (src.locus == Locus::Explicit || src.locus == Locus::User)
        return Trusted{};

    // Project / Local: rode in on the workspace. Trusted ONLY when THIS exact
    // content hash has been approved — swap the bytes and the approval is void
    // (the MCPoison / CVE-2025-54136 fix, stated as behaviour). An empty hash
    // means "nothing executable to vouch for" and is treated as approved.
    if (content_sha.empty() || approvals.approved(content_sha))
        return Trusted{};
    return Pending{std::string{content_sha}};
}

}  // namespace agentty::scope
