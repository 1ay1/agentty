#pragma once
// agentty::tools::util::handoff — enforce the trust-handoff policy on writes.
//
// The missing half of `sandbox_cfg::HandoffPolicy`. The pane has offered the
// row since the provenance module landed, the config persisted it, and nothing
// read it: `is_host_trusted()` had zero callers outside its own test. A
// security control that saves and enforces nothing is the exact failure this
// subsystem exists to prevent, one level up -- the switch said `refuse` and
// the write went through.
//
// ── Why this is not inside the sandbox ──────────────────────────────────
//
// Because the sandbox cannot see it. Every wall below (landlock, seccomp,
// cgroup) confines the PROCESS, and a trust handoff is not a process doing
// something forbidden: it is the agent writing a perfectly ordinary file,
// inside the workspace it is allowed to write, which a component OUTSIDE the
// sandbox later executes. The mount namespace is the wrong altitude. By the
// time VS Code runs .vscode/tasks.json there is no sandbox involved at all.
//
// So the gate sits where the authority actually is: on the tool call, before
// the bytes land. That is also why it cannot be a seccomp rule -- see
// sandbox_broker.hpp on why a path-based decision from a syscall argument is a
// TOCTOU bug rather than a policy.
//
// ── Why it checks the TOOL and not the sandbox's write log ──────────────
//
// `Provenance` records what the sandboxed process wrote, which is the right
// data for an audit trail and the wrong data for a gate: it is after the fact.
// The agent's file-authoring tools (write/edit/apply_patch/move) are in-process
// and never touch the sandbox at all, so the sandbox's log would not even see
// the Cursor-hooks case. Checking the argument of the call that is about to run
// catches both, and catches them before the write rather than after.
//
// ── The policy, and why Refuse is the default ──────────────────────────
//
//   Refuse  the call fails with a named error the model can read and route
//           around ("write the script, ask the user to wire it up").
//   Warn    the call proceeds; the handoff is recorded so the UI can say so.
//   Allow   no gate. For a user whose workflow genuinely needs the agent to
//           author hooks, chosen explicitly rather than inherited.
//
// Refuse is the default because this is not a "might be risky" prompt. Every
// escape in Pillar's July 2026 series worked through this shape and none of
// them needed to break a sandbox, so the cost of a false negative is an escape
// and the cost of a false positive is one declined tool call with a message
// saying exactly what to do instead.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/sandbox_provenance.hpp"

namespace agentty::tools::util::handoff {

// What the gate decided about one write.
struct Verdict {
    bool allowed = true;

    // Set whenever the path IS a trust handoff, whether or not it was allowed
    // -- a Warn decision is still an event worth surfacing. Empty when the
    // path is ordinary, which is the overwhelmingly common case.
    bool is_handoff = false;
    sandbox_cfg::TrustKind kind{};

    // Why, in the words the model and the user both read. Empty when allowed
    // and ordinary.
    std::string reason;
};

// Decide about one path.
//
// Pure apart from reading the sealed sandbox config: no filesystem access, so
// a path that does not exist yet (the usual case for a write) decides the same
// as one that does. `tool` names the caller for the message only.
[[nodiscard]] Verdict check(std::string_view path, std::string_view tool);

// Decide using an explicit policy instead of the live config. The testable
// seam, and the one the parity tests drive.
[[nodiscard]] Verdict check_with(std::string_view path, std::string_view tool,
                                 sandbox_cfg::HandoffPolicy policy);

// Every handoff the gate has seen this session, newest last, bounded.
//
// Same shape and the same reasoning as the broker's blocked feed: written from
// tool worker threads, read from the reducer thread, so it is `jaal::guarded`
// internally and handed back by value.
[[nodiscard]] std::vector<sandbox_cfg::TrustHandoff> handoff_feed();

// Forget everything recorded. For tests.
void clear_handoff_feed();

// ── The shell hole, and why this half exists ─────────────────────────
//
// check() guards write/edit/apply_patch/move, which each hand over a PATH we
// can inspect. `shell` hands over an opaque string, so the gate above never
// ran for it -- and `cat > .vscode/tasks.json` walked straight past a policy
// that correctly knew the path was dangerous. Measured: `write` to that path
// is refused, the same bytes via shell are not.
//
// Three mechanisms were considered. The notes are in
// docs/design/shell-write-gate.md; the short version:
//
//   PARSE the command -- rejected as the mechanism. Catching `>` means also
//     catching `tee`, `dd of=`, `sh -c`, `python -c`, heredocs, a path in a
//     variable, base64-then-decode. mcp-cpp's bash_validate.hpp already
//     carries the scars: its first version matched `sed -i` and `cat > f` as
//     READS. A sniffer is defence in depth, never the wall.
//
//   LANDLOCK a read-only carve -- real, measured, and unshippable here. Within
//     one ruleset rights INHERIT and a deeper stricter rule does not subtract
//     (so the obvious carve is ignored); across layers they INTERSECT, which
//     does work -- but rules target DIRECTORIES only, and granting the
//     ancestors on the way to the carve re-opens it. Carving .vscode
//     therefore makes loose files in the workspace root read-only. A worse
//     regression than the hole.
//
//   BIND a read-only mount over each trusted path -- the right wall, and the
//     one agentty already uses to mask secrets for exactly this reason: a
//     negative rule has no landlock spelling, so you make the path not be the
//     file. Exact, cheap, grammar-proof. Needs a mount namespace, so it is
//     absent on the hosts that deny unprivileged userns.
//
// Hence this: DETECTION, which holds everywhere including those hosts. It
// cannot prevent the write, and it cannot be evaded by any quoting trick,
// because it observes the filesystem rather than the command. Snapshot the
// trusted-shape paths that exist, run the call, compare. Any change becomes a
// handoff event on the same feed the UI already renders.
//
// Prevention is the stronger guarantee where a mount namespace exists;
// detection is the one that is always true.

// An opaque before-picture of the trusted paths under `root`.
//
// Cheap by construction: it stats only paths matching a trusted SHAPE, never
// walks the tree. An empty snapshot (no trusted paths present, or the gate is
// off) makes the matching review() a no-op.
struct TrustedSnapshot {
    struct Entry {
        std::string            path;
        sandbox_cfg::TrustKind kind{};
        std::uintmax_t         size = 0;
        std::int64_t           mtime_ns = 0;
        bool                   existed = false;
    };
    std::vector<Entry> entries;

    // Directories that were walked and could sprout a NEW trusted file.
    // review() re-scans these, because `entries` only covers paths that
    // already existed -- and a freshly created hook is the more dangerous
    // case, not the lesser one.
    std::vector<std::string> roots;

    [[nodiscard]] bool empty() const noexcept {
        return entries.empty() && roots.empty();
    }
};

// Snapshot before a shell-ish call. Returns {} when the policy is Allow or the
// sandbox is off -- there is nothing to report, so there is no reason to stat.
[[nodiscard]] TrustedSnapshot snapshot_trusted(std::string_view root);

// Compare after the call and record a handoff for every path that changed,
// appeared or vanished. Returns the number recorded.
//
// Deliberately does NOT fail the call. By the time this runs the bytes are
// already on disk, so refusing would be theatre; the honest outcome is a loud,
// attributable event. `tool` names the caller for the message.
std::size_t review_trusted(const TrustedSnapshot& before, std::string_view tool);

}  // namespace agentty::tools::util::handoff
