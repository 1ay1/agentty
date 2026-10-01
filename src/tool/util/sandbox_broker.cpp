// The syscall supervisor's decision policy. Scalars only — see the header for
// why that constraint is structural rather than a simplification.

#include "agentty/tool/util/sandbox_broker.hpp"

#include <jaal/kernel/guarded.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <signal.h>
#endif

namespace agentty::tools::util::sandbox::broker {

namespace {

#if defined(__linux__)

// ptrace(2) requests we are willing to reason about. The rest are denied, not
// because each is dangerous but because a supervisor that allows requests it
// has not thought about is not a policy.
//
// PTRACE_TRACEME is the interesting case: it is how a child ASKS to be traced
// by its own parent, which is exactly what `strace prog` and a debugger-under-
// test do. It grants the tracer no new authority -- the child is volunteering.
[[nodiscard]] const char* ptrace_request_name(std::uint64_t req) {
    switch (req) {
        case PTRACE_TRACEME:     return "TRACEME";
        case PTRACE_ATTACH:      return "ATTACH";
        case PTRACE_SEIZE:       return "SEIZE";
        case PTRACE_PEEKDATA:    return "PEEKDATA";
        case PTRACE_POKEDATA:    return "POKEDATA";
        case PTRACE_CONT:        return "CONT";
        case PTRACE_DETACH:      return "DETACH";
        case PTRACE_GETREGSET:   return "GETREGSET";
        case PTRACE_SETOPTIONS:  return "SETOPTIONS";
        default:                 return "?";
    }
}

[[nodiscard]] std::string signal_name(std::uint64_t sig) {
    switch (sig) {
        case 0:        return "0";        // existence probe, sends nothing
        case SIGTERM:  return "SIGTERM";
        case SIGKILL:  return "SIGKILL";
        case SIGINT:   return "SIGINT";
        case SIGHUP:   return "SIGHUP";
        case SIGUSR1:  return "SIGUSR1";
        case SIGUSR2:  return "SIGUSR2";
        case SIGCHLD:  return "SIGCHLD";
        case SIGSTOP:  return "SIGSTOP";
        case SIGCONT:  return "SIGCONT";
        default:       return std::to_string(sig);
    }
}

#endif  // __linux__

}  // namespace

namespace {

// The blocked-activity feed's storage.
//
// `jaal::guarded<T>` rather than a raw std::mutex, and not merely because the
// concurrency banlist says so (it does -- tests/lint/allowlist.txt, and my
// first version failed that check). The type is better for the job: access is
// only possible through `with()`, so "forgot the lock" is unrepresentable
// rather than a review item. For a cross-thread security surface that is worth
// more than the two lines it saves.
//
// Function-local static so initialisation order cannot bite: the first denial
// can arrive from a worker thread during startup, and a file-scope global would
// be a race with its own constructor.
jaal::guarded<std::vector<Event>>& feed() {
    static jaal::guarded<std::vector<Event>> f;
    return f;
}

// How many distinct events to keep.
//
// Bounded because a guest hammering a denied syscall in a loop would otherwise
// grow this without limit -- a denial of service the supervisor inflicts on its
// own host. 64 is chosen for the READER, not for memory: a pane showing more
// than a screenful of denials has stopped communicating, and the coalescing
// below means 64 distinct events covers far more than 64 syscalls.
constexpr std::size_t kMaxEvents = 64;

}  // namespace

void record(const Event& ev) {
    // Allows are not blocked activity. The broker logs both -- the log is a
    // trace -- but a feed that lists every permitted syscall is an strace, and
    // the one denial that matters would drown in it.
    if (ev.allowed) return;

    feed().with([](std::vector<Event>& events, Event incoming) {
        // Coalesce an identical repeat. Checked against the LAST event only,
        // not the whole feed: a build that alternates two denied calls should
        // show both interleaved rather than two counters, because the order is
        // the information -- it tells you what the toolchain was doing.
        if (!events.empty()) {
            auto& last = events.back();
            if (last.syscall == incoming.syscall &&
                last.detail == incoming.detail) {
                // Saturate rather than wrap. A count that rolls over to 0 would
                // read as "this never happened", which is the worst possible
                // lie for an audit surface to tell.
                if (last.count < UINT32_MAX) ++last.count;
                return;
            }
        }

        incoming.count = 1;
        events.push_back(std::move(incoming));

        // Drop the OLDEST when full, keeping the newest. A guest trying to
        // flush evidence of an early denial has to push 64 DISTINCT denials
        // through, every one of which is itself recorded -- so the attempt is
        // louder than the thing it would hide.
        if (events.size() > kMaxEvents) events.erase(events.begin());
    }, ev);
}

std::vector<Event> blocked_feed() {
    // A copy, deliberately: the caller is on another thread, so handing back a
    // reference would be a race with the next denial. `with()` makes that hard
    // to get wrong -- the result must not point into the guarded value.
    return feed().with([](std::vector<Event>& events) { return events; });
}

void clear_blocked_feed() {
    feed().with([](std::vector<Event>& events) { events.clear(); });
}

std::vector<std::uint32_t> brokered_syscalls() {
#if defined(__linux__) && defined(SYS_ptrace) && defined(SYS_kill)
    // Exactly two, and the shortness of this list is the design.
    //
    // A syscall belongs here only if ALL of:
    //   - a scalar argument carries the decision (no pointer, no TOCTOU)
    //   - a flat deny is genuinely wrong sometimes (so brokering buys
    //     compatibility, not just a log line)
    //   - a flat allow is genuinely wrong sometimes (so there is a decision
    //     to make at all)
    //
    // ptrace and kill meet all three. Nothing involving a path does, because
    // the path is a pointer; those belong in landlock. Nothing involving
    // sockets does yet either -- socket() is scalar-decidable and claybin even
    // ships a helper, but the network namespace already gives a stronger,
    // race-free answer, and two mechanisms for one boundary is how they drift.
    return {static_cast<std::uint32_t>(SYS_ptrace),
            static_cast<std::uint32_t>(SYS_kill)};
#else
    return {};
#endif
}

Verdict decide(std::uint32_t nr, const std::uint64_t args[6],
               std::uint32_t guest_pid, std::uint32_t guest_pgid,
               Event* out_event) {
    auto record = [&](const char* sc, std::string detail, bool allowed) {
        if (out_event) {
            out_event->syscall = sc;
            out_event->detail = std::move(detail);
            out_event->allowed = allowed;
        }
        return allowed ? Verdict::Allow : Verdict::Deny;
    };

#if defined(__linux__) && defined(SYS_ptrace) && defined(SYS_kill)
    if (nr == static_cast<std::uint32_t>(SYS_ptrace)) {
        const std::uint64_t request = args[0];
        const auto target = static_cast<std::uint32_t>(args[1]);
        const char* rname = ptrace_request_name(request);
        std::string detail = std::string{rname} +
                             " pid=" + std::to_string(target);

        // An unknown request is denied REGARDLESS of target.
        //
        // Checked before the target rules, and that order is the fix for a bug
        // this file shipped: the target check came first, so an unrecognised
        // request aimed at our own group was allowed. There are ~40 ptrace
        // requests, several of them (PTRACE_SETREGS, PTRACE_POKEUSER) are how
        // you rewrite a traced process's execution, and "it is one of ours" is
        // not a reason to permit an operation nobody reasoned about. A
        // supervisor that allows requests it does not understand is not a
        // policy.
        if (std::string_view{rname} == "?")
            return record("ptrace", std::move(detail), false);

        // TRACEME: the caller volunteers to be traced by its own parent. No
        // new authority over anything else, and it is how `strace prog` works
        // from the inside. Allowed.
        if (request == PTRACE_TRACEME)
            return record("ptrace", std::move(detail), true);

        // Everything else names a TARGET, and the target is a scalar, so this
        // is exactly the shape brokering is sound for.
        //
        // Allowed only inside our own process group. A debugger attaching to
        // its own child is ordinary; reaching outside the group is the shape of
        // an escape -- and pid 1 inside a pid namespace is the sandbox's own
        // init, which is a container-escape primitive rather than a debugging
        // step.
        //
        // Deliberately NOT "same pid namespace": we cannot ask the kernel that
        // from a register value, and guessing would be the kind of almost-right
        // check this file exists to avoid. The process group is something we
        // know for certain because we set it.
        if (target == 0 || target == guest_pid)
            return record("ptrace", std::move(detail), true);
        if (guest_pgid != 0 && target == guest_pgid)
            return record("ptrace", std::move(detail), true);
        return record("ptrace", std::move(detail), false);
    }

    if (nr == static_cast<std::uint32_t>(SYS_kill)) {
        // kill(pid, sig). Both scalars.
        const auto target = static_cast<std::int32_t>(
            static_cast<std::int64_t>(args[0]));
        const std::uint64_t sig = args[1];
        std::string detail = signal_name(sig) + " pid=" + std::to_string(target);

        // Self, and the own-group forms (0 = my group, negative = that group).
        // A build killing its own children is routine: `make` on failure,
        // `timeout`, a test runner reaping workers.
        if (target == 0 || target == static_cast<std::int32_t>(guest_pid))
            return record("kill", std::move(detail), true);
        if (target < 0) {
            const auto grp = static_cast<std::uint32_t>(-target);
            if (grp == guest_pgid || grp == guest_pid)
                return record("kill", std::move(detail), true);
            return record("kill", std::move(detail), false);
        }
        if (guest_pgid != 0 && static_cast<std::uint32_t>(target) == guest_pgid)
            return record("kill", std::move(detail), true);

        // kill(-1, sig) is "every process I may signal". Inside a pid
        // namespace that is bounded, but we do not get to assume we have one
        // (see §3a: namespaces may be denied while seccomp still works), so it
        // is denied.
        return record("kill", std::move(detail), false);
    }
#else
    (void)nr; (void)args; (void)guest_pid; (void)guest_pgid;
#endif

    // A syscall we asked to broker and then did not handle. Denying is the only
    // safe default -- allowing would mean the brokered list and this function
    // can drift apart silently, in the permissive direction.
    return record("?", "unhandled brokered syscall", false);
}

}  // namespace agentty::tools::util::sandbox::broker
