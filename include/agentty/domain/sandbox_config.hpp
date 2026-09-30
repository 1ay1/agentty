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

struct Config {
    // ── Filesystem ───────────────────────────────────────────────────────
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

    // ── Resources ────────────────────────────────────────────────────────
    // cgroup2 when the host delegates, rlimit as a backstop otherwise. The
    // guarantee report distinguishes the two rather than claiming both.
    //
    // 0 = no cap, which is today's behaviour for all three.
    std::uint64_t memory_mb   = 0;
    std::uint32_t max_procs   = 0;
    std::uint32_t cpu_percent = 0;   // 100 = one core
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

    // Has the user ever saved sandbox settings? While false the runtime keeps
    // its shipped posture, so an upgrade changes nothing until a deliberate
    // edit -- the same contract RagConfig::configured has.
    bool configured = false;
};

}  // namespace agentty::sandbox_cfg
