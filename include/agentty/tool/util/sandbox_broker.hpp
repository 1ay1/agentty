#pragma once
// agentty::tools::util::sandbox::broker — the syscall supervisor.
//
// This is what turns the sandbox from a FILTER into a CAPABILITY SYSTEM. A
// filter answers "may you call ptrace"; a supervisor answers "may you ptrace
// THIS pid", at runtime, with the arguments in hand.
//
// ── Why this file is small, and must stay small ──────────────────────────
//
// seccomp user-notification is the single sharpest footgun in the Linux
// sandboxing API, and the kernel documentation says so outright: "it should be
// absolutely clear that this means the seccomp notifier cannot be used to
// implement a security policy" — about SECCOMP_USER_NOTIF_FLAG_CONTINUE
// specifically. Two hazards, and both are structural rather than bugs to be
// careful about.
//
// 1. TOCTOU on pointer arguments. A notification carries the syscall's
//    REGISTER values. A pointer argument names memory in the guest, and the
//    guest has other threads: it can rewrite that memory between the moment we
//    read it and the moment the kernel acts. So reading a path out of the guest
//    and then allowing the call is a vulnerability, not a feature. Checkpoint/
//    restore and container runtimes have shipped this bug repeatedly.
//
// 2. CONTINUE is not a decision. Telling the kernel to resume the syscall
//    re-runs it with the GUEST's credentials and the guest's view of memory,
//    after our check. Everything checked is stale by then.
//
// claybin's broker refuses to expose either hazard: a Decision is allow, deny,
// or inject_fd (we did the work, the guest gets a descriptor). There is no
// `continue`, and there is no `path` field on a Request. This file therefore
// has exactly one job — decide from SCALARS — and the moment a rule here wants
// to look at a path, that rule belongs in landlock instead, which the kernel
// enforces with no race at all.
//
// ── What brokering is FOR here ──────────────────────────────────────────
//
// Not filesystem rules: landlock already does those, better. Brokering earns
// its place on the calls where a scalar decision is genuinely useful and a
// blanket deny is genuinely wrong:
//
//   ptrace    a debugger attaching to its OWN child is normal (gdb, lldb,
//             strace under a test). Attaching to anything else is the shape of
//             an escape. The target pid is a SCALAR, so this is decidable.
//   kill      signalling your own process group is normal; signalling pid 1 or
//             something outside the sandbox is not. Also scalars.
//
// Both are currently flat denials in the compiler profile, which is safe and
// occasionally wrong — `cargo test` under a debugger, a test suite that
// strace's itself. Brokering turns "denied, good luck" into "denied, and the
// log says it tried to attach to pid 1".
//
// ── The audit trail is half the point ───────────────────────────────────
//
// Every decision is recorded. "Your build failed" teaches nothing; "cargo
// tried ptrace(PTRACE_ATTACH, 1) and was denied" teaches what your toolchain
// does, and is the difference between adding one allowlist line and turning
// the sandbox off.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace agentty::tools::util::sandbox::broker {

// One decision we made, for the record.
struct Event {
    std::string syscall;   // "ptrace", "kill"
    std::string detail;    // "PTRACE_ATTACH pid=1", "SIGKILL pid=1"
    bool allowed = false;
    std::uint32_t count = 1;   // coalesced repeats of the same decision
};

// What a supervisor decided, in agentty's terms. Deliberately NOT claybin's
// Decision: this header is included by the tool layer, which has no business
// knowing claybin's types, and the mapping is one switch in the .cpp.
enum class Verdict : std::uint8_t {
    Allow,   // sound only when decided from scalars alone
    Deny,    // EPERM; always sound
};

// The policy. Pure: scalars in, verdict out, no I/O and no guest memory.
//
// `nr` is the syscall number for the HOST architecture, `args` its six register
// arguments verbatim, `guest_pid` the thread that made the call.
//
// Pure on purpose, so it is testable without a kernel: sandbox_broker_test
// drives it with the exact argument shapes a real ptrace/kill carries, which is
// the only way to check the interesting cases (attach to pid 1, signal outside
// the group) without arranging a real escape attempt.
[[nodiscard]] Verdict decide(std::uint32_t nr, const std::uint64_t args[6],
                             std::uint32_t guest_pid, std::uint32_t guest_pgid,
                             Event* out_event);

// Which syscalls agentty asks claybin to broker. Empty when brokering is off.
//
// Returned as data rather than applied in place so the posture builder stays
// the single place that mutates a policy, and so a test can assert the list
// without building a policy at all.
[[nodiscard]] std::vector<std::uint32_t> brokered_syscalls();

}  // namespace agentty::tools::util::sandbox::broker
