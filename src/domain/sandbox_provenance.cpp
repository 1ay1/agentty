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
#include <chrono>
#include <filesystem>
#include <span>

#include <maya/runtime.hpp>

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
// Two kinds of rule, told apart by an explicit marker:
//
//   SUFFIX  `*.pem`, `*.tfvars` — matches any file ENDING in the extension.
//           These are why the walk had to exist at all: a suffix cannot be
//           turned into a path without looking at what is actually on disk.
//   NAME    everything else matches the whole basename, exactly.
//
// The marker is explicit rather than inferred from shape. Inferring "starts
// with a dot and has no second dot" looks tidy and is wrong: it silently
// promotes `.env`, `.npmrc` and `.netrc` to suffixes, so `prod.env` and
// `scoped.npmrc` start being masked too. Widening what a SECURITY rule covers
// as a side effect of adding an unrelated entry is exactly the kind of
// accident this list must not have -- a mask that appears from nowhere is as
// confusing as one that goes missing, and nobody reviewing the list would see
// it.
[[nodiscard]] bool is_masked_name(std::string_view name) {
    for (const char* n : kAlwaysMaskedNames) {
        std::string_view rule{n};
        if (rule.starts_with("*")) {
            rule.remove_prefix(1);                 // "*.pem" -> ".pem"
            // `>` not `>=`, so the rule never matches a file that IS the bare
            // suffix: a file called ".pem" is not a key.
            if (name.size() > rule.size() && name.ends_with(rule)) return true;
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

// A fingerprint of the DIRECTORY SHAPE that `sweep` above traverses: the
// mtime of every directory it would descend into, folded together.
//
// Why this is the right key for sweep's cache, and why it is exact rather
// than approximate: creating, deleting or renaming an entry updates the
// mtime of the directory CONTAINING it. That is the only mutation that can
// change which files sweep returns -- editing a file's CONTENTS cannot add
// or remove it from the list, and that is the operation whose parent mtime
// does not move. So "same stamp" really does imply "same answer".
//
// It must mirror sweep's traversal EXACTLY -- same depth arithmetic, same
// never_walk skips, same no-follow rule. A stamp that visited fewer
// directories than the sweep would be blind to changes inside the ones it
// skipped, which is a stale mask, which is a hole. Both functions therefore
// take the same shape deliberately; change one and you must change the other.
//
// Cheap on purpose: one status() per DIRECTORY, no readdir of its entries
// beyond what the iterator already yields, and no per-file symlink_status.
std::uint64_t dir_stamp(const std::filesystem::path& dir, int depth_left) {
    std::error_code ec;
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a offset basis
    const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };

    const auto mt = std::filesystem::last_write_time(dir, ec);
    if (!ec)
        mix(static_cast<std::uint64_t>(mt.time_since_epoch().count()));

    std::filesystem::directory_iterator it{
        dir, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) return h;

    for (const auto& entry : it) {
        if (depth_left <= 0) break;
        std::error_code sec;
        const auto st = entry.symlink_status(sec);
        if (sec || !std::filesystem::is_directory(st)) continue;
        const auto name = entry.path().filename().string();
        if (never_walk(name)) continue;
        mix(dir_stamp(entry.path(), depth_left - 1));
    }
    return h;
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
        // CACHED, because this is in the latency path of EVERY command.
        //
        // The header already warned that an unbounded walk here "would put a
        // full directory scan in the latency path of every shell command" and
        // bounded the depth in response. Bounding was not enough: measured on
        // the agentty tree at the shipped depth=3, the sweep costs 3.7 ms and
        // runs again for every single spawn -- re-deriving a byte-identical
        // answer each time. Against a ~10 ms sandboxed `true`, that is most
        // of the floor, and it grows with the repo rather than with the work.
        //
        // THE KEY IS THE TREE'S OWN SHAPE, not a clock.
        //
        // A time-to-live was the obvious first try and it is WRONG here: it
        // means a credential that appears inside the TTL is readable by the
        // next command. sandbox_live_check caught it immediately -- it plants
        // `services/api/key.pem` and runs a command against it in well under
        // a second, and the stale list let the body through. A mask that is
        // merely eventually-correct is not a wall, and a 2-second hole is
        // exactly long enough for the `write file` → `run tool` pair an agent
        // does constantly.
        //
        // So key on the mtimes of every DIRECTORY the sweep descends into.
        // Creating, deleting or renaming a file bumps its parent's mtime --
        // that is the one thing POSIX guarantees here, and it is precisely
        // the event that can change this list. Stat-ing the directories costs
        // a fraction of walking their entries (one syscall per directory, no
        // readdir, no per-file symlink_status), so the common case stays
        // cheap while staying exact.
        // maya::guarded rather than a raw std::mutex: the concurrency banlist
        // requires it (tests/lint/allowlist.txt -- my first version failed
        // that check), and the type is the better tool anyway. The state is
        // only reachable through with(), so "forgot the lock" is
        // unrepresentable instead of a review item. Commands spawn from tool
        // worker threads, so this really is shared.
        struct Cache {
            std::string              ws;
            int                      depth = -1;
            std::uint64_t            stamp = 0;
            std::vector<std::string> paths;
        };
        // Function-local static: the first command can land from a worker
        // during startup, and a file-scope global would race its own ctor.
        static maya::guarded<Cache> cache;

        const int depth = static_cast<int>(cfg.mask_scan_depth);
        // OUTSIDE the lock: stat-ing the tree is the slow part, and holding
        // the lock across it would serialise every concurrent spawn behind
        // one filesystem walk -- turning a latency fix into a latency bug.
        const std::uint64_t stamp =
            dir_stamp(std::filesystem::path{workspace}, depth);

        // Captureless, as guarded<T> insists: everything it needs is passed
        // as an argument. (A capture could name a second lock, and holding
        // two is how deadlocks start -- the type enforces that rather than
        // trusting the comment.)
        //
        // Returns a COPY rather than appending through a reference: with()
        // forwards its arguments, so an out-parameter would bind to an
        // rvalue. Copying ~16 short strings once per command is nothing
        // against the walk this exists to avoid, and it keeps the critical
        // section to a memcpy.
        auto hits = cache.with(
            [](Cache& c, std::string ws_, int depth_, std::uint64_t stamp_) {
                if (c.ws != ws_ || c.depth != depth_ || c.stamp != stamp_) {
                    c.paths.clear();
                    sweep(std::filesystem::path{ws_}, depth_, c.paths);
                    c.ws    = std::move(ws_);
                    c.depth = depth_;
                    c.stamp = stamp_;
                }
                return c.paths;
            },
            std::string{workspace}, depth, stamp);
        out.insert(out.end(), std::make_move_iterator(hits.begin()),
                   std::make_move_iterator(hits.end()));
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
            // The fork-bomb cap SURVIVES Permissive, and it is the one
            // resource wall that does.
            //
            // memory/cpu/nofile/time all go to 0 here for the reason the field
            // comments give: the right ceiling is a property of the machine and
            // the build, so a wrong guess turns a working `cargo build` into an
            // OOM kill that looks like agentty's fault. Dropping them is what
            // makes Permissive a real answer to "my toolchain breaks".
            //
            // max_procs is different, and the field's own comment says why:
            // "unlike memory, there IS a number that is safe everywhere. No
            // legitimate build needs 4096 concurrent processes; `make -j` on a
            // 64-core box peaks in the low hundreds, and the pathological case
            // is not 'a big build' but `:(){ :|:& };:`, which wants millions."
            //
            // It was being zeroed with the rest, which handed every Permissive
            // user an unbounded fork bomb from any approved command -- the
            // exact finding tests/sandbox_audit.cpp was written for (it saw
            // resource.pids as `none` and that is why 4096 became the default),
            // reintroduced through the preset. claybin treats 0 as unlimited
            // and sets no rlimit and no pids.max at all, so this was not a
            // softer cap; it was no cap.
            //
            // Permissive means "the walls are off", not "the machine is
            // forfeit". A fork bomb is not a toolchain compatibility problem,
            // so there is nothing for this to unbreak.
            c.max_procs = 4096;
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
