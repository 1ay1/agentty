// sandbox_provenance.cpp — WHICH path shapes the host later trusts.
//
// The matching moved to claybin (clay::shape_matches, policy/path_shape.hpp)
// because it is general mechanism with a sharp edge: whole-component rather
// than substring, or a rule fires on ".gitignore" and gets ignored; and
// shape rather than exact name, or `git --git-dir=.foo` walks past it, which
// is the bug Cursor shipped and fixed in 3.0.0.
//
// What stays here is the only part that is agentty's opinion: the TABLE. Every
// entry is a place a documented escape landed, or the obvious sibling of one.
// claybin has no view on whether ".vscode/tasks.json" matters -- that is a
// statement about how developer tooling behaves, not about the kernel.

#include "agentty/domain/sandbox_provenance.hpp"

#include <array>
#include <span>

// The matcher lives in claybin. A build without it still needs this file to
// compile -- the trust-handoff TABLE is agentty's, the boundary is a product
// decision, and a default build that cannot even name the risk is worse than
// one that names it and says the enforcement is unavailable.
//
// So: the same table either way, with a local fallback matcher when claybin is
// absent. The fallback is deliberately the SAME shape logic, not a looser one,
// because a classification that differs by build flag is a bug waiting for
// whoever tests the other configuration.
#if defined(AGENTTY_HAVE_CLAYBIN)
#include "claybin/policy/path_shape.hpp"
#endif

namespace agentty::sandbox_cfg {

namespace {

#if defined(AGENTTY_HAVE_CLAYBIN)

using clay::ShapeMatch;
using clay::ShapeRule;
using clay::shape_matches;

#else

// The same three modes and the same matching, for a build without claybin.
//
// Duplicated deliberately rather than shared through a third header: it is
// twenty lines, and the alternative is agentty growing a path-utilities module
// that exists only to be included by two files. If these ever disagree the
// tests catch it -- the trust-handoff cases run in both configurations.
enum class ShapeMatch : std::uint8_t { Component, Basename, Suffix };

struct ShapeRule {
    std::string_view needle;
    std::uint32_t tag = 0;
    ShapeMatch match = ShapeMatch::Component;
};

[[nodiscard]] std::string_view basename_of(std::string_view p) {
    const auto slash = p.rfind('/');
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

[[nodiscard]] bool has_component(std::string_view p, std::string_view needle) {
    if (needle.empty()) return false;
    std::size_t pos = 0;
    while (pos <= p.size()) {
        const std::size_t next = p.find('/', pos);
        const std::string_view comp =
            p.substr(pos, next == std::string_view::npos ? p.size() - pos : next - pos);
        if (comp == needle) return true;
        if (needle.find('/') != std::string_view::npos &&
            p.compare(pos, needle.size(), needle) == 0) {
            const std::size_t after = pos + needle.size();
            if (after == p.size() || p[after] == '/') return true;
        }
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return false;
}

[[nodiscard]] bool shape_matches(std::string_view p, std::span<const ShapeRule> rules,
                                 std::uint32_t* out_tag = nullptr) {
    const std::string_view base = basename_of(p);
    for (const auto& r : rules) {
        bool hit = false;
        switch (r.match) {
            case ShapeMatch::Component: hit = has_component(p, r.needle); break;
            case ShapeMatch::Basename:  hit = base == r.needle; break;
            case ShapeMatch::Suffix:
                hit = base.size() > r.needle.size() && base.ends_with(r.needle);
                break;
        }
        if (hit) {
            if (out_tag) *out_tag = r.tag;
            return true;
        }
    }
    return false;
}

#endif

// tag == TrustKind, cast through the opaque integer claybin carries.
constexpr std::uint32_t tag(TrustKind k) { return static_cast<std::uint32_t>(k); }

// Ordered: first match wins, so the more specific reason comes first where a
// path could plausibly hit two rules.
constexpr std::array kRules{
    // ── lifecycle hooks: CVE-2026-48124 (Cursor, CVSS 8.5) ──────────────
    ShapeRule{"hooks.json", tag(TrustKind::HookConfig), ShapeMatch::Basename},
    ShapeRule{".claude", tag(TrustKind::HookConfig), ShapeMatch::Component},
    ShapeRule{".agentty", tag(TrustKind::HookConfig), ShapeMatch::Component},
    ShapeRule{".cursor", tag(TrustKind::HookConfig), ShapeMatch::Component},

    // ── editor automation: Antigravity's ".vscode time bomb" ────────────
    ShapeRule{".vscode", tag(TrustKind::EditorTask), ShapeMatch::Component},
    ShapeRule{"tasks.json", tag(TrustKind::EditorTask), ShapeMatch::Basename},
    ShapeRule{"launch.json", tag(TrustKind::EditorTask), ShapeMatch::Basename},
    ShapeRule{".idea", tag(TrustKind::EditorTask), ShapeMatch::Component},

    // ── git: fsmonitor + hooks + config (Cursor 3.0.0) ──────────────────
    //
    // ".git" as a COMPONENT so a renamed git dir does not slip past, and the
    // internals by basename because the directory can be called anything
    // while `config` and `HEAD` cannot.
    ShapeRule{".git", tag(TrustKind::GitConfig), ShapeMatch::Component},
    ShapeRule{"config", tag(TrustKind::GitConfig), ShapeMatch::Basename},
    ShapeRule{"HEAD", tag(TrustKind::GitConfig), ShapeMatch::Basename},
    ShapeRule{"hooks", tag(TrustKind::GitConfig), ShapeMatch::Component},

    // ── interpreters and entry points: GHSA-p9g2-cr55-cw9c ──────────────
    //
    // The Python extension executed a venv interpreter during discovery. Any
    // bin/ under the workspace has the same property.
    ShapeRule{"node_modules/.bin", tag(TrustKind::Interpreter), ShapeMatch::Component},
    ShapeRule{"pyvenv.cfg", tag(TrustKind::Interpreter), ShapeMatch::Basename},
    ShapeRule{".venv", tag(TrustKind::Interpreter), ShapeMatch::Component},
    ShapeRule{"venv", tag(TrustKind::Interpreter), ShapeMatch::Component},

    // ── shell init: runs on the user's next shell here ──────────────────
    ShapeRule{".envrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".bashrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".zshrc", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{"activate", tag(TrustKind::ShellInit), ShapeMatch::Basename},
    ShapeRule{".direnv", tag(TrustKind::ShellInit), ShapeMatch::Component},

    // ── build scripts: run on the next build, which is soon ─────────────
    ShapeRule{"Makefile", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"package.json", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"build.rs", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"CMakeLists.txt", tag(TrustKind::BuildScript), ShapeMatch::Basename},
    ShapeRule{"conftest.py", tag(TrustKind::BuildScript), ShapeMatch::Basename},

    // ── daemon sockets: GHSA-v4xv-rqh3-w9mc ─────────────────────────────
    //
    // "One Docker socket to rule them all" escaped Codex, Cursor AND Gemini
    // CLI. A privileged local daemon is a second execution environment.
    ShapeRule{"docker.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
    ShapeRule{"containerd.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
    ShapeRule{"podman.sock", tag(TrustKind::DaemonSocket), ShapeMatch::Basename},
};

}  // namespace

bool is_host_trusted(std::string_view path, TrustKind* out_kind) {
    std::uint32_t t = 0;
    if (!shape_matches(path, std::span<const ShapeRule>{kRules}, &t)) return false;
    if (out_kind) *out_kind = static_cast<TrustKind>(t);
    return true;
}

}  // namespace agentty::sandbox_cfg
