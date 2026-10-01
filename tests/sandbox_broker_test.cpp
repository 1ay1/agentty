// The syscall supervisor's decision policy.
//
// Unit-testable because `decide()` is pure: scalars in, verdict out, no I/O and
// no guest memory. That is the whole reason it is shaped that way -- the
// interesting cases are "attach to pid 1" and "signal outside the group", and
// arranging those against a real kernel would mean staging a real escape
// attempt. Here they are three lines each.
//
// What this canNOT check is that the supervisor is wired, polled, and answers
// in time. A policy that decides perfectly and is never asked is a hang. That
// is sandbox_live_check's job (cases 9-11), and the two together are what make
// brokering safe to ship.

#include <doctest/doctest.h>

#include "agentty/tool/util/sandbox_broker.hpp"

#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <signal.h>

namespace br = agentty::tools::util::sandbox::broker;

namespace {

// Our guest: pid 4242, leading its own process group (claybin calls setsid, so
// the child leads a group and every descendant inherits it -- that is what
// makes "inside my own group" decidable from a register value).
constexpr std::uint32_t kGuest = 4242;
constexpr std::uint32_t kGroup = 4242;

br::Verdict ptrace_call(std::uint64_t request, std::uint64_t target,
                        br::Event* ev = nullptr) {
    const std::uint64_t args[6] = {request, target, 0, 0, 0, 0};
    br::Event local;
    return br::decide(static_cast<std::uint32_t>(SYS_ptrace), args, kGuest,
                      kGroup, ev ? ev : &local);
}

br::Verdict kill_call(std::int64_t target, std::uint64_t sig,
                      br::Event* ev = nullptr) {
    const std::uint64_t args[6] = {static_cast<std::uint64_t>(target), sig,
                                   0, 0, 0, 0};
    br::Event local;
    return br::decide(static_cast<std::uint32_t>(SYS_kill), args, kGuest,
                      kGroup, ev ? ev : &local);
}

}  // namespace

TEST_CASE("broker: only scalar-decidable syscalls are brokered") {
    // The list is short BY DESIGN, and this pins why. A syscall belongs there
    // only if a scalar carries the decision -- anything taking a path is a
    // pointer, and reading guest memory then approving the call is the TOCTOU
    // bug the kernel documentation warns about. Those rules belong in landlock,
    // which the kernel enforces with no race.
    const auto list = br::brokered_syscalls();
    REQUIRE(list.size() == 2);
    CHECK(list[0] == static_cast<std::uint32_t>(SYS_ptrace));
    CHECK(list[1] == static_cast<std::uint32_t>(SYS_kill));
}

TEST_CASE("broker: PTRACE_TRACEME is allowed") {
    // The child volunteering to be traced by its own parent. It grants no
    // authority over anything else, and it is how `strace prog` works from the
    // inside -- the compiler profile KILLS ptrace, so without brokering every
    // debugger and every strace-under-test dies outright.
    br::Event ev;
    CHECK(ptrace_call(PTRACE_TRACEME, 0, &ev) == br::Verdict::Allow);
    CHECK(ev.allowed);
    CHECK(ev.syscall == "ptrace");
    // The audit line names the request, because "ptrace denied" is useless and
    // "ptrace TRACEME allowed" is actionable.
    CHECK(ev.detail.find("TRACEME") != std::string::npos);
}

TEST_CASE("broker: attaching outside our own group is denied") {
    // pid 1 inside a pid namespace is the sandbox's own init: attaching to it
    // is a container-escape primitive, not a debugging step. The target is a
    // SCALAR, which is exactly why this is soundly decidable -- no guest memory
    // is read, so there is no window to race.
    br::Event ev;
    CHECK(ptrace_call(PTRACE_ATTACH, 1, &ev) == br::Verdict::Deny);
    CHECK(!ev.allowed);
    CHECK(ev.detail.find("ATTACH") != std::string::npos);
    CHECK(ev.detail.find("pid=1") != std::string::npos);

    // An unrelated pid, likewise.
    CHECK(ptrace_call(PTRACE_ATTACH, 99999) == br::Verdict::Deny);
    CHECK(ptrace_call(PTRACE_SEIZE, 99999) == br::Verdict::Deny);
}

TEST_CASE("broker: attaching within our own group is allowed") {
    // A debugger attaching to its own child is ordinary. `cargo test` under
    // gdb, a test suite that straces a helper it spawned.
    CHECK(ptrace_call(PTRACE_ATTACH, kGuest) == br::Verdict::Allow);
    CHECK(ptrace_call(PTRACE_ATTACH, kGroup) == br::Verdict::Allow);
    // Target 0 means "self" for the requests that accept it.
    CHECK(ptrace_call(PTRACE_ATTACH, 0) == br::Verdict::Allow);
}

TEST_CASE("broker: killing our own group is allowed") {
    // Routine: make on failure, timeout(1), a test runner reaping workers.
    CHECK(kill_call(0, SIGTERM) == br::Verdict::Allow);               // my group
    CHECK(kill_call(kGuest, SIGTERM) == br::Verdict::Allow);          // myself
    CHECK(kill_call(-static_cast<std::int64_t>(kGroup), SIGKILL) ==
          br::Verdict::Allow);                                        // my group
}

TEST_CASE("broker: killing outside our group is denied") {
    br::Event ev;
    CHECK(kill_call(1, SIGKILL, &ev) == br::Verdict::Deny);
    CHECK(!ev.allowed);
    CHECK(ev.detail.find("SIGKILL") != std::string::npos);

    CHECK(kill_call(99999, SIGTERM) == br::Verdict::Deny);
    CHECK(kill_call(-99999, SIGTERM) == br::Verdict::Deny);

    // kill(-1) is "every process I may signal". Bounded inside a pid
    // namespace, but §3a means we may not HAVE one -- namespaces can be denied
    // while seccomp still works -- so assuming one would be exactly the
    // almost-right check this policy exists to avoid.
    CHECK(kill_call(-1, SIGKILL) == br::Verdict::Deny);
}

TEST_CASE("broker: signal 0 follows the same rules as a real signal") {
    // kill(pid, 0) sends nothing and is the standard existence probe. It is
    // still an information leak about processes outside the sandbox, and it
    // costs nothing to treat it like any other target, so the rule is uniform
    // rather than special-cased. A reader should not have to remember an
    // exception.
    CHECK(kill_call(kGuest, 0) == br::Verdict::Allow);
    CHECK(kill_call(1, 0) == br::Verdict::Deny);
}

TEST_CASE("broker: an unhandled brokered syscall is denied") {
    // The fail-safe direction. If brokered_syscalls() ever grows an entry that
    // decide() does not handle, the two have drifted -- and the default must be
    // deny, because the alternative is a silent widening that no test would
    // notice.
    const std::uint64_t args[6] = {0, 0, 0, 0, 0, 0};
    br::Event ev;
    // SYS_getpid is never brokered, so this stands in for "a number decide()
    // has no case for".
    CHECK(br::decide(static_cast<std::uint32_t>(SYS_getpid), args, kGuest,
                     kGroup, &ev) == br::Verdict::Deny);
    CHECK(!ev.allowed);
}

TEST_CASE("broker: an unknown ptrace request is denied") {
    // Not because each unknown request is dangerous, but because a supervisor
    // that allows requests it has not reasoned about is not a policy. There are
    // ~40 ptrace requests and the ones we have thought about are listed.
    CHECK(ptrace_call(0xdead, kGuest) == br::Verdict::Deny);
}
