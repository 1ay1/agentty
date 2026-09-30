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
#include "agentty/domain/sandbox_config.hpp"   // Config, kAlwaysMasked*, mask_paths

#include <algorithm>
#include <array>
#include <filesystem>
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

namespace {

// Directories never worth walking for a credential file.
//
// Two properties, and BOTH are needed to justify skipping one: it is reliably
// enormous (so walking it costs real time on every spawn), and a credential
// file does not legitimately live there. The second is the safety argument --
// if a name on this list ever becomes somewhere secrets DO live, skipping it
// stops being a performance decision and becomes a hole.
//
// .git is the interesting case: it holds no `.env`, but it DOES hold
// `.git/config`, which can carry a credential helper. That is covered by the
// trust-handoff rules above (TrustKind::GitConfig) rather than by masking, so
// skipping it here loses nothing.
constexpr std::string_view kNeverWalk[] = {
    ".git", "node_modules", "target", "build", "dist", ".venv", "venv",
    "__pycache__", ".mypy_cache", ".pytest_cache", ".cache", ".next",
    ".gradle", ".tox", "vendor", "Pods", ".terraform",
};

[[nodiscard]] bool never_walk(std::string_view name) {
    for (auto d : kNeverWalk) if (name == d) return true;
    return false;
}

// Does this basename name a credential file?
//
// `.pem` is a SUFFIX rule rather than a name, and it is the reason the walk
// had to exist at all: a suffix cannot be turned into a path without looking
// at what is actually on disk.
[[nodiscard]] bool is_masked_name(std::string_view name) {
    for (const char* n : kAlwaysMaskedNames) {
        const std::string_view rule{n};
        if (rule == ".pem") {
            if (name.size() > 4 && name.ends_with(".pem")) return true;
            continue;
        }
        if (name == rule) return true;
    }
    return false;
}

// Walk `dir` to `depth_left` levels, appending every file whose basename is a
// credential name.
//
// Recursive, and safe to be: the depth is bounded by mask_scan_depth before
// the first call, and directory symlinks are never followed, so no input can
// drive this deeper than the bound.
void sweep(const std::filesystem::path& dir, int depth_left,
           std::vector<std::string>& out) {
    std::error_code ec;
    // skip_permission_denied: a workspace can contain directories we cannot
    // read, and a masking sweep must not fail the whole spawn over one of
    // them. Directory symlinks are NOT followed -- following one out of the
    // workspace would mask host paths the user never asked about, and one
    // pointing back inside would walk the tree twice.
    std::filesystem::directory_iterator it{
        dir, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) return;

    for (const auto& entry : it) {
        std::error_code sec;
        const auto name = entry.path().filename().string();
        // symlink_status, not status: a symlink NAMED .env is masked as the
        // link rather than followed. Following it would mask wherever it
        // points and leave the link itself readable.
        const auto st = entry.symlink_status(sec);
        if (sec) continue;

        if (std::filesystem::is_directory(st)) {
            if (depth_left > 0 && !never_walk(name))
                sweep(entry.path(), depth_left - 1, out);
            continue;
        }
        if (is_masked_name(name)) out.push_back(entry.path().string());
    }
}

}  // namespace

std::vector<std::string> mask_paths(const Config& cfg, std::string_view workspace,
                                   std::string_view home) {
    std::vector<std::string> out;

    // 1. $HOME credentials. Not configurable, by design: a control the user
    //    can switch off to make their build work is a control that is off.
    if (!home.empty()) {
        const std::string h{home};
        for (const char* m : kAlwaysMasked) out.push_back(h + m);
    }

    // 2. Credential names inside the workspace, found by walking. The
    //    workspace is bound READ-WRITE by both backends, so scope cannot save
    //    anything in here and the mask is the only wall.
    if (!workspace.empty()) {
        sweep(std::filesystem::path{workspace},
              static_cast<int>(cfg.mask_scan_depth), out);
    }

    // 3. The user's own denials, last, so an explicit deny cannot be undone
    //    by one of ours.
    for (const auto& d : cfg.deny_paths) out.push_back(d);

    // Deduplicate: a path can arrive from the sweep and from deny_paths, and
    // emitting the same mount twice is at best noise in the plan. Sorting is
    // also what makes the argv stable across runs -- a test asserting order
    // would otherwise be flaky on directory-iteration order.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace agentty::sandbox_cfg
