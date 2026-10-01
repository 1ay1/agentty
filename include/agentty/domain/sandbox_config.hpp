#pragma once
// agentty::sandbox_cfg — the user-facing sandbox policy, as a domain type.
//
// WHAT THIS IS FOR. agentty's sandbox posture used to be a set of constants in
// sandbox.cpp: which system roots are readable, whether the network is shared,
// what /tmp is sized at. Reasonable defaults, but not a policy a user could
// state — and "can this agent reach the internet" is exactly the question a
// user has an opinion about and no way to express.
//
// Everything here is enforced by claybin. That is the point of having picked a
// library over a binary: bwrap can express the mounts, but a per-port network
// grant, a syscall filter and a memory cap are things you can only ask for if
// you are compiling a policy rather than assembling an argv.
//
// ── The shape follows smart::RoleConfig ──────────────────────────────────
// A DOMAIN struct that store::Settings composes whole, so the settings
// registry can bind rows straight into the thing the runtime reads. The
// alternative -- flat fields on Settings -- makes "network off but allowlist
// populated" representable and forces a hand-written mapping at every
// boundary, which is a place to forget a field. (store/store.hpp says this
// out loud about the eleven fields RoleConfig replaced.)
//
// ── Why the defaults are what they are ──────────────────────────────────
// They reproduce TODAY's behaviour exactly. A user upgrading gets the sandbox
// they already had; every stricter setting is opt-in. A security default that
// breaks `npm install` on upgrade does not get left on, it gets turned off
// along with everything else in the panel.

#include <cstdint>
#include <string>
#include <vector>

#include "agentty/domain/sandbox_provenance.hpp"

namespace agentty::sandbox_cfg {

// How much of the filesystem the sandbox exposes read-only.
//
// This is the axis every agent sandbox gets wrong in the same direction: the
// convenient answer is "bind / read-only" and it hands over ~/.ssh, ~/.aws and
// every other project on the machine. agentty's own history has one -- a
// backend that granted read on `/` while reporting "sandbox: active".
enum class FsScope : std::uint8_t {
    // /usr, /bin, /lib, /etc's specific files, and the $HOME toolchain dirs
    // (~/.cargo, ~/.nvm, ~/.local/bin ...). What a build needs and nothing
    // else. This is today's behaviour.
    Toolchain,
    // The workspace, the loader, and /usr. No toolchain dirs -- for a
    // self-contained repo where anything under $HOME is suspicious.
    Minimal,
    // Everything under / readable. Honest name for the convenient answer:
    // still a mount namespace, still no writes outside the workspace, but
    // your keys are readable by an approved command.
    HostReadable,
};

[[nodiscard]] constexpr const char* to_string(FsScope s) noexcept {
    switch (s) {
        case FsScope::Toolchain:    return "toolchain";
        case FsScope::Minimal:      return "minimal";
        case FsScope::HostReadable: return "host-readable";
    }
    return "toolchain";
}

// What the sandboxed command may reach on the network.
//
// The research consensus (Claude Code's allowlist proxy, Codex's
// workspace-write network toggle, innoq's proxy writeup) is that a blanket
// on/off is too coarse: agents legitimately need registry and git traffic, and
// that is also the exfiltration path. Everyone reaches for an allowlist.
//
// claybin can enforce this IN THE KERNEL rather than through a proxy, because
// landlock (abi 4+) mediates TCP connect per port. That is a genuinely
// stronger boundary than a proxy an agent could be talked into bypassing --
// but it is PORT granularity, not hostname, so it is honest about being a
// different tool rather than a better proxy.
enum class NetMode : std::uint8_t {
    // Share the host's network namespace. Today's behaviour: git push, npm
    // install and curl all work, and so does anything else.
    Full,
    // An empty network namespace. No sockets at all, not even loopback DNS.
    // The strictest setting, and it breaks any command that fetches.
    None,
    // Only the ports in `allow_ports`, enforced by landlock. Needs landlock
    // abi >= 4; falls back to Full with the guarantee report saying `partial`
    // rather than silently pretending.
    Ports,
};

[[nodiscard]] constexpr const char* to_string(NetMode m) noexcept {
    switch (m) {
        case NetMode::Full:  return "full";
        case NetMode::None:  return "none";
        case NetMode::Ports: return "ports";
    }
    return "full";
}

// How aggressively syscalls are filtered.
//
// bwrap takes --seccomp FD and agentty passed it nothing, so until now there
// was no syscall filter at all. These are claybin's profiles.
enum class SyscallMode : std::uint8_t {
    // No filter. What agentty did before claybin, kept so a user whose
    // toolchain breaks under a filter has somewhere to land that is not
    // "sandbox off".
    Off,
    // profiles::compiler_with_network(). Files, subprocesses, sockets; denies
    // ptrace, mount, unshare, setns, bpf, kexec and the module calls; filters
    // clone's namespace flags by ARGUMENT; W^X on mmap/mprotect; ioctl reduced
    // to an allow-list that excludes the TIOCSTI keystroke-injection family.
    Compiler,
    // profiles::with_filesystem() -- no subprocess creation. Breaks any build
    // that shells out, which is most of them. For read-only analysis runs.
    Strict,
};

[[nodiscard]] constexpr const char* to_string(SyscallMode m) noexcept {
    switch (m) {
        case SyscallMode::Off:      return "off";
        case SyscallMode::Compiler: return "compiler";
        case SyscallMode::Strict:   return "strict";
    }
    return "compiler";
}

// Paths that are NEVER exposed, whatever the scope says.
//
// Today these are absent by accident: the toolchain scope binds ~/.cargo/bin
// rather than ~/.cargo, so credentials never come along. That is the right
// outcome from a careful read set, but it is fragile -- it holds because
// somebody chose the narrow path, and the next person adding "~/.config" for
// some tool's config would take ~/.config/gh with it.
//
// Every source in the 2026 sandbox literature lists secret hygiene as a
// separate control for this reason ("no ambient production secrets"), so it
// gets to be one: a deny list applied AFTER every grant, including
// HostReadable and including anything the user typed into read_paths.
//
// Landlock makes this cheap and exact -- a deny rule on a subtree beats the
// grants above it -- which is a thing a mount-only sandbox cannot do without
// masking each path with a bind.
inline constexpr const char* kAlwaysMasked[] = {
    // credentials, in rough order of how badly you would mind losing them
    "/.ssh",
    "/.aws",
    "/.gnupg",
    "/.kube",
    "/.docker/config.json",
    "/.config/gh",           // github cli token
    "/.config/gcloud",
    "/.azure",
    "/.netrc",
    "/.npmrc",               // npm auth token
    "/.pypirc",
    "/.cargo/credentials",
    "/.cargo/credentials.toml",
    "/.git-credentials",
    // agentty's own state: the memory store can contain anything the user
    // told it, and the credential store obviously counts.
    "/.agentty/memory.jsonl",
    "/.agentty/credentials.json",
};

// Basenames masked anywhere they appear, including inside the workspace.
//
// A .env in the repo is the single most common place a real secret sits, and
// the workspace is READ-WRITE -- so unlike the list above, scope does not save
// us here. Matched on basename because .env lives wherever the framework put
// it.
inline constexpr const char* kAlwaysMaskedNames[] = {
    ".env",
    ".env.local",
    ".env.production",
    ".envrc.local",
    "id_rsa",
    "id_ed25519",
    ".pem",  // suffix, handled as such by the matcher
};

// Which Linux engine applies the policy.
//
// A RUNTIME choice, not a build one: both backends are always compiled in
// (claybin is a required submodule). It lives in the saved config because it
// decides what the rest of the config can even mean -- bwrap confines with
// mount namespaces and nothing else, so the syscall profile, per-port
// network, W^X and the cgroup caps have no bwrap spelling at all.
enum class LinuxBackend : std::uint8_t {
    // bubblewrap, via the binary the user already has. What agentty shipped
    // first, and still the default: a decade of upstream hardening, and it
    // does not change anyone's boundary on upgrade.
    Bwrap,
    // The in-process library. Adds a seccomp filter, landlock and cgroup2
    // limits -- i.e. everything the pane's other rows describe.
    Claybin,
};

[[nodiscard]] constexpr const char* to_string(LinuxBackend b) noexcept {
    switch (b) {
        case LinuxBackend::Bwrap:   return "bwrap";
        case LinuxBackend::Claybin: return "claybin";
    }
    return "bwrap";
}

// A named posture: the whole policy as one decision.
//
// ── Why this exists ────────────────────────────────────────────────
//
// The pane has 28 rows. Every one of them is a real, enforced, individually
// justified control -- and asking a user to assemble a security boundary out
// of 28 primitives is still the wrong question. Nobody opens this screen
// wanting to choose a pids cgroup limit. They open it wanting one of about
// four things, and the honest UI is the one that asks which.
//
// This is the same lesson the rest of the pane already learned about honesty,
// applied to effort: a control nobody can reason about is a control nobody
// uses, and an unused security control is off. 28 correct switches that all
// stay at their defaults forever are worth less than one choice a user
// actually makes.
//
// ── Why presets and not a wizard ──────────────────────────────────
//
// Because the rows stay. A preset WRITES the 28 values and leaves every one of
// them visible and editable -- it is a starting point, not a mode. The moment a
// preset hid the detail it would become another "sandbox: active": a label
// standing in for a boundary you can no longer inspect.
//
// So `Custom` is not a preset you pick, it is what the pane REPORTS when the
// config matches none of the others. Tweaking one row off Hardened does not
// silently keep claiming Hardened.
enum class Posture : std::uint8_t {
    // No sandbox beyond the mount namespace. For a user whose toolchain
    // genuinely breaks under a filter and who would otherwise pass
    // --sandbox off, which is strictly worse.
    Permissive,
    // The default, and what `configured = false` means: toolchain-readable,
    // network on, compiler syscall profile, fork-bomb cap. Chosen so an
    // ordinary `cargo build` / `npm install` works untouched.
    Balanced,
    // Every wall claybin can build, with resource caps that still let a real
    // build finish. The posture to pick if you are running agents you have
    // not read.
    Hardened,
    // No network at all, and the tightest filter. For reading and editing
    // code without letting anything phone out -- the posture that makes
    // "read access plus network is read plus exfiltrate" (§11) untrue.
    Airgapped,
    // Not selectable. What the pane says when the rows match no preset.
    Custom,
};

[[nodiscard]] constexpr const char* to_string(Posture p) noexcept {
    switch (p) {
        case Posture::Permissive: return "permissive";
        case Posture::Balanced:   return "balanced";
        case Posture::Hardened:   return "hardened";
        case Posture::Airgapped:  return "airgapped";
        case Posture::Custom:     return "custom";
    }
    return "custom";
}

// One line on what the posture gives up, for the row's help text. Phrased as
// the COST rather than the benefit: every one of these is safe-sounding, and
// the thing a user needs to know before picking is what will stop working.
[[nodiscard]] constexpr const char* cost_of(Posture p) noexcept {
    switch (p) {
        case Posture::Permissive:
            return "no syscall filter, no caps \xc2\xb7 only the mount walls";
        case Posture::Balanced:
            return "network is open, so read access is also exfiltration";
        case Posture::Hardened:
            return "a build that needs an unusual syscall may fail";
        case Posture::Airgapped:
            return "git push, npm install and curl all stop working";
        case Posture::Custom:
            return "your own mix \xc2\xb7 the rows below are the truth";
    }
    return "";
}

struct Config {
    // ── Engine ────────────────────────────────────────────────────────
    // First field because it gates the meaning of most of the others. The
    // pane renders it first and locks the rows the chosen backend cannot
    // enforce, rather than accepting a setting that does nothing.
    LinuxBackend backend = LinuxBackend::Bwrap;

    // ── Filesystem ───────────────────────────────────────────────────
    FsScope fs_scope = FsScope::Toolchain;

    // Extra paths, beyond the scope above. Read-only unless listed in
    // `write_paths`. A user with a monorepo dependency outside the workspace,
    // or a shared cache, needs this and has no other way to say it.
    std::vector<std::string> read_paths;
    // Writable paths beyond the workspace. Deliberately separate from
    // read_paths: granting write is a different decision from granting read,
    // and a single list would make the dangerous one the easy one.
    std::vector<std::string> write_paths;
    // Paths to mask even inside the granted scope. The escape hatch for
    // "toolchain scope, but not ~/.cargo/credentials".
    std::vector<std::string> deny_paths;

    // ── Network ──────────────────────────────────────────────────────────
    NetMode net_mode = NetMode::Full;
    // Ports allowed under NetMode::Ports. 443 and 80 cover HTTPS and HTTP;
    // 22 is git-over-ssh; 53 is DNS, which is easy to forget and breaks
    // everything when you do.
    std::vector<std::uint16_t> allow_ports{443, 80, 22, 53};

    // ── Syscalls ─────────────────────────────────────────────────────────
    SyscallMode syscall_mode = SyscallMode::Compiler;
    // Refuse a mapping that is both writable and executable. Cheap, and it
    // breaks the whole class of exploits that rely on one RWX page -- but a
    // JIT (node with some flags, any JVM) genuinely needs it.
    bool wx_protect = true;

    // ── Resources ────────────────────────────────────────────────────
    // cgroup2 when the host delegates, rlimit as a backstop otherwise. The
    // guarantee report distinguishes the two rather than claiming both.
    //
    // memory and cpu default to NO CAP, deliberately: the right ceiling is a
    // property of the machine and the build, and a wrong one turns a working
    // `cargo build` into an OOM kill that looks like agentty's fault. There is
    // no number we can pick for someone else's 8-core laptop and 64-core
    // workstation.
    std::uint64_t memory_mb   = 0;
    std::uint32_t cpu_percent = 0;   // 100 = one core

    // Processes is DIFFERENT, and ships with a real cap.
    //
    // The audit (tests/sandbox_audit.cpp) showed resource.pids as `none` on a
    // default install, which means a fork bomb from an approved command was
    // unbounded -- and unlike memory, there IS a number that is safe
    // everywhere. No legitimate build needs 4096 concurrent processes; `make
    // -j` on a 64-core box peaks in the low hundreds, and the pathological
    // case is not "a big build" but `:(){ :|:& };:`, which wants millions.
    //
    // So this is the one resource wall that can be on by default without
    // guessing about the host. The others stay off until the user picks a
    // number, because for them a guess is worse than nothing.
    std::uint32_t max_procs   = 4096;
    // Open descriptors (RLIMIT_NOFILE). A separate lever from max_procs: a
    // runaway that leaks fds exhausts the host's file table without ever
    // forking, so a pid cap does not bound it.
    std::uint32_t max_open_files = 0;
    // Wall-clock ceiling for one command, in seconds. Distinct from the tool
    // layer's own timeout: this one is enforced by the sandbox, so it still
    // applies to a child that ignores SIGTERM or wedges in a syscall.
    std::uint32_t wall_clock_secs = 0;
    // CPU-time ceiling (RLIMIT_CPU), in seconds. Bounds total compute rather
    // than elapsed time, so a process that sleeps forever is unaffected while
    // a busy loop is killed. Separate from cpu_percent, which throttles
    // instead of killing.
    std::uint32_t cpu_secs = 0;
    // /tmp size. Enforced as a mount option, so a runaway build hits ENOSPC
    // inside the sandbox instead of filling the host's RAM. bwrap cannot
    // express this at all.
    std::uint64_t tmp_mb = 512;

    // ── Hardening ────────────────────────────────────────────────────────
    // Landlock scoping (abi 6+): closes abstract unix sockets and
    // cross-boundary signals. These ignore the filesystem entirely -- an
    // abstract socket has no path -- so nothing else in this struct covers
    // them. Off means "rely on the network namespace", which stops working
    // the moment net_mode is Full.
    bool scope_ipc = true;
    // Close descriptors inherited from agentty. An inherited fd is authority
    // the sandbox cannot see or revoke; bubblewrap leaks one here.
    bool close_inherited_fds = true;

    // Hide the host's identity from the guest.
    //
    // A distinct concern from the filesystem: the hostname and the guest's
    // apparent uid/gid leak into build output, test fixtures and anything that
    // shells out to `id`. Neither is a capability, so this is about
    // reproducibility and fingerprinting rather than containment -- which is
    // why it is a separate row and defaults to off.
    bool fake_hostname = false;   // claim "sandbox" instead of the real host

    // Depth for the credential-name sweep (kAlwaysMaskedNames).
    //
    // Those are BASENAMES -- `.env`, `id_rsa` -- and a name rule cannot be
    // turned into a mount without knowing where the file is, so the paths have
    // to be found by walking. Only the workspace root was swept before, which
    // missed the common case: `services/api/.env` in any monorepo.
    //
    // Bounded rather than recursive-to-the-leaves, and the bound is a setting
    // because the right value depends on the repo. Every extra level is more
    // stat() calls on every spawn, and an unbounded walk of a big tree would
    // put a directory scan in the latency path of every shell command.
    //
    // 0 = root only (the old behaviour). 3 covers the layouts that actually
    // occur -- services/*/.env, packages/*/.env, apps/*/web/.env.
    std::uint32_t mask_scan_depth = 3;

    // Has the user ever saved sandbox settings? While false the runtime keeps
    // its shipped posture, so an upgrade changes nothing until a deliberate
    // edit -- the same contract RagConfig::configured has.
    bool configured = false;

    // ── Trust handoff ────────────────────────────────────────────────────
    //
    // What to do when the agent writes a file the HOST later executes -- a
    // hooks config, a .vscode task, a git config, a venv interpreter. This is
    // the shape of every escape in Pillar Security's July 2026 series
    // (CVE-2026-48124 and friends): the agent never broke the sandbox, it just
    // wrote something a trusted component outside the sandbox ran later.
    //
    // Refuse by default, which is stricter than how anything else here
    // behaves. An approval prompt is right for "this might be risky"; a trust
    // handoff is the shape of a known escape class, so the answer starts at no.
    HandoffPolicy handoff = HandoffPolicy::Refuse;

    // Defaulted equality, so the Sandbox pane can ask "is the saved policy
    // different from the one actually enforcing?" and answer honestly.
    //
    // That question only exists because the live policy is SEALED at startup
    // (see tool/util/sandbox.hpp): a save during the session goes to disk and
    // does not take effect, so the pane has two configs to compare and has to
    // tell the user when they diverge. Before the seal there was only one.
    //
    // Defaulted rather than hand-written on purpose: a hand-rolled comparison
    // that forgets a field would report "no change" for a policy that did
    // change, which is the wrong direction for a security control to fail.
    // This way adding a field to the struct keeps the comparison complete.
    [[nodiscard]] bool operator==(const Config&) const = default;
};

// Every absolute path that must be masked, for a given config and workspace.
//
// ONE function, used by BOTH backends. That is the point: the credential list
// is the least negotiable part of the policy, and a list applied by one
// backend and not the other is how `.env` leaked under bwrap for a week while
// reading back empty under claybin. If this is wrong it is wrong everywhere,
// which is much easier to notice.
//
// Three sources, in order of how much they are the user's business:
//
//   1. kAlwaysMasked      — $HOME credential paths. Not configurable.
//   2. kAlwaysMaskedNames — credential BASENAMES, found by walking the
//                           workspace to cfg.mask_scan_depth. Not
//                           configurable except for the depth.
//   3. cfg.deny_paths     — whatever the user added. Last, so an explicit
//                           deny cannot be undone by one of ours.
//
// Why the walk exists: a basename rule cannot become a mount without knowing
// where the file is. Masking only `<workspace>/.env` missed
// `services/api/.env`, which is where it actually lives in any monorepo.
//
// Why it is BOUNDED: this runs on every spawn, so an unbounded walk of a large
// tree would put a full directory scan in the latency path of every shell
// command. Depth-limited, and it skips the directories that are always huge
// and never hold secrets (.git, node_modules, target, build) — skipping those
// is a performance decision, and it is safe only because none of them is
// somewhere a credential file legitimately lives. Revisit that if the list
// ever grows a directory where one might.
//
// `$HOME` comes in as a parameter rather than being read from the environment,
// so this stays pure and testable without touching a real home directory.
[[nodiscard]] std::vector<std::string> mask_paths(
    const Config& cfg, std::string_view workspace, std::string_view home);

// ── Postures ───────────────────────────────────────────────────

// Write a posture's values over a config, preserving the parts that are the
// USER's rather than the posture's.
//
// What survives: the path lists (read/write/deny) and the engine. A posture is
// a statement about how tight the walls are, not about which extra dependency
// directory this particular project needs -- wiping someone's `read_paths`
// because they tried Hardened would make the presets hostile to use.
//
// What a posture does NOT touch is `backend`. Picking Hardened on a host where
// claybin is unavailable must not silently switch engines; the pane locks that
// row and reports it, which is the honest answer.
[[nodiscard]] Config apply_posture(const Config& base, Posture p);

// Which posture this config IS, or Custom.
//
// Derived by comparison rather than stored, and that is the load-bearing
// choice: a STORED posture field would drift from the rows the moment a user
// edited one, and then the pane would be claiming a boundary the config no
// longer describes -- the same lie as "sandbox: active". Deriving it means the
// label cannot be wrong, only "Custom".
[[nodiscard]] Posture detect_posture(const Config& cfg);

}  // namespace agentty::sandbox_cfg
