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

Config apply_posture(const Config& base, Posture p) {
    Config c = base;
    c.configured = true;

    // Deliberately NOT touched: backend, read_paths, write_paths, deny_paths.
    // A posture is a statement about how tight the walls are; the engine and
    // the user's project-specific path list are not the posture's business.
    // See the header.

    switch (p) {
        case Posture::Permissive:
            // Off, not "loose". There is no filter, no caps and no netns --
            // only the mount walls, which both backends always build. This
            // exists so that a user whose toolchain breaks has somewhere to go
            // that is not `--sandbox off`: the masks and the mount namespace
            // still hold, which is strictly more than nothing.
            c.fs_scope = FsScope::Toolchain;
            c.net_mode = NetMode::Full;
            c.syscall_mode = SyscallMode::Off;
            c.wx_protect = false;
            c.memory_mb = 0;
            c.max_procs = 0;
            c.cpu_percent = 0;
            c.max_open_files = 0;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = false;
            c.close_inherited_fds = true;   // never a compatibility problem
            c.fake_hostname = false;
            c.mask_scan_depth = 3;          // secrets stay masked REGARDLESS
            // Handoff stays at the caller's value rather than being loosened.
            // Permissive is about the walls around the process; the handoff
            // gate is about what the agent writes for the HOST to run, and
            // those are different questions (§14). A loose sandbox is a
            // choice; letting the agent author your git hooks as a side effect
            // of that choice is not one anybody made.
            break;

        case Posture::Balanced:
            // Exactly the struct defaults, by construction: this is what
            // `configured = false` already means, so a user who picks Balanced
            // explicitly gets the same boundary as one who never opened the
            // pane. Writing them out rather than default-constructing keeps
            // the four postures readable side by side.
            c.fs_scope = FsScope::Toolchain;
            c.net_mode = NetMode::Full;
            c.syscall_mode = SyscallMode::Compiler;
            c.wx_protect = true;
            c.memory_mb = 0;        // the machine's business, not ours
            c.max_procs = 4096;     // fork-bomb cap; the audit found this none
            c.cpu_percent = 0;
            c.max_open_files = 0;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = false;
            c.mask_scan_depth = 3;
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Hardened:
            // Every wall claybin can build, with caps a real build survives.
            //
            // The numbers are the interesting part. They are chosen to be
            // survivable rather than impressive: 8 GB and 400% (four cores)
            // because a linker peaks high and an OOM kill mid-build reads as
            // an agentty bug, and no wall-clock cap at all because a long
            // build is not an attack and a timeout that fires on one teaches
            // the user to turn the sandbox off.
            c.fs_scope = FsScope::Minimal;
            c.net_mode = NetMode::Ports;   // 443/80/22/53 by default
            c.syscall_mode = SyscallMode::Strict;
            c.wx_protect = true;
            c.memory_mb = 8192;
            c.max_procs = 2048;
            c.cpu_percent = 400;
            c.max_open_files = 4096;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = true;        // keeps the host name out of logs
            c.mask_scan_depth = 5;         // deeper sweep; it is a cost row
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Airgapped:
            // The posture that makes §11's admission untrue.
            //
            // Everywhere else in this subsystem, network is open and the
            // documentation says plainly that read access plus network is read
            // plus exfiltrate. This is the one setting where that stops being
            // true, which is why it is worth a preset even though it breaks
            // most workflows: for reading and editing code it costs nothing,
            // and it is the only configuration here where a leaked credential
            // cannot leave the machine.
            c.fs_scope = FsScope::Minimal;
            c.net_mode = NetMode::None;
            c.syscall_mode = SyscallMode::Strict;
            c.wx_protect = true;
            c.memory_mb = 8192;
            c.max_procs = 2048;
            c.cpu_percent = 400;
            c.max_open_files = 4096;
            c.wall_clock_secs = 0;
            c.cpu_secs = 0;
            c.scope_ipc = true;
            c.close_inherited_fds = true;
            c.fake_hostname = true;
            c.mask_scan_depth = 5;
            c.handoff = HandoffPolicy::Refuse;
            break;

        case Posture::Custom:
            // Not selectable: it is what detect_posture REPORTS, not a thing
            // to apply. Returning the config untouched is the only sound
            // answer -- there is nothing to write.
            break;
    }
    return c;
}

Posture detect_posture(const Config& cfg) {
    // By COMPARISON against what each posture would produce, not by inspecting
    // fields one at a time.
    //
    // This is why the label cannot be wrong. apply_posture is the single
    // definition of what a posture means, and detect asks it rather than
    // re-encoding the same knowledge in a second place that can drift. Add a
    // field to Config and this keeps working; add a field and hand-write the
    // comparison, and the day you forget one the pane starts claiming
    // "Hardened" for a config that is not.
    //
    // `configured` is normalised out of the comparison on both sides. It is
    // bookkeeping about whether the user has ever touched the pane, not part of
    // the boundary -- and apply_posture always sets it, so without this a
    // never-opened config (configured = false) would report Custom even though
    // its walls are exactly Balanced. The pane would open on "Custom" for every
    // new user, which is both wrong and the least useful thing it could say.
    Config probe = cfg;
    probe.configured = true;

    // Order matters only for display: Balanced is checked first because it is
    // the overwhelmingly common answer, so the common case costs one compare.
    for (auto p : {Posture::Balanced, Posture::Hardened, Posture::Airgapped,
                   Posture::Permissive}) {
        if (probe == apply_posture(probe, p)) return p;
    }
    return Posture::Custom;
}

}  // namespace agentty::sandbox_cfg
