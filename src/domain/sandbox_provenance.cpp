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

// The matcher lives in claybin, which is a required submodule -- so this is a
// plain include, not a guarded one.
//
// This used to carry a second, local copy of the same shape logic for builds
// without claybin, on the reasoning that the trust-handoff TABLE is agentty's
// opinion and should compile either way. That reasoning was sound and the
// conclusion was wrong: it meant sixty lines of duplicated matching, two
// configurations where a classification could differ, and a comment admitting
// the duplication was "deliberate". With claybin required there is exactly one
// matcher, which is what the boundary audit wanted in the first place.
#include "claybin/policy/path_shape.hpp"

namespace agentty::sandbox_cfg {

namespace {

using clay::ShapeMatch;
using clay::ShapeRule;
using clay::shape_matches;

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
