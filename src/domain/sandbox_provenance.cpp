// sandbox_provenance.cpp — is this path something the host later trusts?
//
// Table-driven and matched on SHAPE, not on exact names. That choice is the
// whole lesson of "Git directories do not have to be called .git" (Pillar,
// fixed in Cursor 3.0.0): the sandbox matched `.git` literally, and
// `git --git-dir=anything` walked straight past it. An exact-name denylist
// against a flexible operator is always one entry short -- which is also
// Pillar's Failure Mode 1, and the reason claybin's ioctl policy is an
// allow-list rather than a deny-list.
//
// So the rules here describe what a path IS, not what it is called:
//   * a component anywhere in the path (".git/", "node_modules/.bin/")
//   * a basename with meaning wherever it appears ("tasks.json", ".envrc")
//   * an executable under a directory that discovery scans (venv "bin/")
//
// None of this is a substitute for the sandbox. It is the answer to the one
// question a mount namespace cannot answer: of the writes we allowed, which
// ones did the HOST just become responsible for executing?

#include "agentty/domain/sandbox_provenance.hpp"

#include <algorithm>
#include <array>

namespace agentty::sandbox_cfg {

namespace {

struct Rule {
    std::string_view needle;
    TrustKind kind;
    // component: needle must be a whole path component (or component prefix
    // ending in '/'). basename: needle must be the final component.
    enum class Match : std::uint8_t { Component, Basename } match;
};

// Kept small and reviewable on purpose. Every entry is a place where a
// documented escape landed, or the obvious sibling of one.
constexpr std::array kRules{
    // ── lifecycle hooks: CVE-2026-48124 (Cursor), and agentty's own ──────
    Rule{"hooks.json", TrustKind::HookConfig, Rule::Match::Basename},
    Rule{".claude", TrustKind::HookConfig, Rule::Match::Component},
    Rule{".agentty", TrustKind::HookConfig, Rule::Match::Component},
    Rule{".cursor", TrustKind::HookConfig, Rule::Match::Component},

    // ── editor automation: Antigravity's ".vscode time bomb" ────────────
    Rule{".vscode", TrustKind::EditorTask, Rule::Match::Component},
    Rule{"tasks.json", TrustKind::EditorTask, Rule::Match::Basename},
    Rule{"launch.json", TrustKind::EditorTask, Rule::Match::Basename},
    Rule{".idea", TrustKind::EditorTask, Rule::Match::Component},

    // ── git: GHSA (Cursor 3.0.0), fsmonitor + hooks + config ────────────
    //
    // ".git" as a COMPONENT, so `--git-dir=.mygit/` does not slip past on a
    // name; and the hook/config leaves by basename, because a git dir can be
    // called anything and its INTERNALS still cannot.
    Rule{".git", TrustKind::GitConfig, Rule::Match::Component},
    Rule{"config", TrustKind::GitConfig, Rule::Match::Basename},
    Rule{"HEAD", TrustKind::GitConfig, Rule::Match::Basename},
    Rule{"hooks", TrustKind::GitConfig, Rule::Match::Component},

    // ── interpreters and entry points: GHSA-p9g2-cr55-cw9c (venv) ───────
    //
    // The Python extension executed a venv interpreter during discovery. Any
    // "bin/" or "Scripts/" under the workspace has the same property.
    Rule{"node_modules/.bin", TrustKind::Interpreter, Rule::Match::Component},
    Rule{"pyvenv.cfg", TrustKind::Interpreter, Rule::Match::Basename},
    Rule{".venv", TrustKind::Interpreter, Rule::Match::Component},
    Rule{"venv", TrustKind::Interpreter, Rule::Match::Component},

    // ── shell init: runs on the user's next shell in this directory ─────
    Rule{".envrc", TrustKind::ShellInit, Rule::Match::Basename},
    Rule{".bashrc", TrustKind::ShellInit, Rule::Match::Basename},
    Rule{".zshrc", TrustKind::ShellInit, Rule::Match::Basename},
    Rule{"activate", TrustKind::ShellInit, Rule::Match::Basename},
    Rule{".direnv", TrustKind::ShellInit, Rule::Match::Component},

    // ── build scripts: run on the next build, which is soon ─────────────
    Rule{"Makefile", TrustKind::BuildScript, Rule::Match::Basename},
    Rule{"package.json", TrustKind::BuildScript, Rule::Match::Basename},
    Rule{"build.rs", TrustKind::BuildScript, Rule::Match::Basename},
    Rule{"CMakeLists.txt", TrustKind::BuildScript, Rule::Match::Basename},
    Rule{"conftest.py", TrustKind::BuildScript, Rule::Match::Basename},

    // ── daemon sockets: Pillar Failure Mode 4, GHSA-v4xv-rqh3-w9mc ──────
    //
    // "One Docker socket to rule them all" escaped Codex, Cursor AND Gemini
    // CLI. A privileged local daemon is a second execution environment: if
    // the agent can talk to it, the daemon does work the agent may not.
    Rule{"docker.sock", TrustKind::DaemonSocket, Rule::Match::Basename},
    Rule{"containerd.sock", TrustKind::DaemonSocket, Rule::Match::Basename},
    Rule{"podman.sock", TrustKind::DaemonSocket, Rule::Match::Basename},
};

[[nodiscard]] std::string_view basename_of(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Does `needle` appear as a whole component of `path`?
//
// Whole-component, so "config" does not match "configure.ac" and ".git" does
// not match ".gitignore" -- the false positives that make a warning people
// learn to ignore.
[[nodiscard]] bool has_component(std::string_view path, std::string_view needle) {
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t next = path.find('/', pos);
        const std::string_view comp =
            path.substr(pos, next == std::string_view::npos ? path.size() - pos : next - pos);
        if (comp == needle) return true;
        // a multi-component needle ("node_modules/.bin") matches as a run
        if (needle.find('/') != std::string_view::npos &&
            path.compare(pos, needle.size(), needle) == 0) {
            const std::size_t after = pos + needle.size();
            if (after == path.size() || path[after] == '/') return true;
        }
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return false;
}

}  // namespace

bool is_host_trusted(std::string_view path, TrustKind* out_kind) {
    const std::string_view base = basename_of(path);
    for (const auto& r : kRules) {
        const bool hit = r.match == Rule::Match::Basename ? base == r.needle
                                                          : has_component(path, r.needle);
        if (hit) {
            if (out_kind) *out_kind = r.kind;
            return true;
        }
    }
    return false;
}

}  // namespace agentty::sandbox_cfg
