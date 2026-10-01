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
#include <fcntl.h>          // open  (reading /proc/<pid>/...)
#include <unistd.h>         // read, close
#include <dirent.h>         // opendir/readdir  (/proc/<pid>/task)
#include <cstdio>           // snprintf
#include <cstdlib>          // strtol
#include <cstring>
#include <vector>
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

// Is `target` a process the REQUESTER may signal -- i.e. itself or one of its
// descendants -- where `target` is a pid as the REQUESTER sees it?
//
// THE GAP THIS CLOSES, and it is two gaps stacked.
//
// (1) Both target rules below used to be equality tests against guest_pid /
//     guest_pgid, which answers "is the target the group LEADER". A child is
//     neither: it has its own pid and inherits the leader's pgid. So every
//     kill(child, sig) was denied -- while the comment two lines away already
//     called that case routine ("`make` on failure, `timeout`, a test runner
//     reaping workers").
//
// (2) The numbers were not even in the same space. claybin gives the guest its
//     own PID NAMESPACE, so the guest is pid 1 in there and the pid it passes
//     to kill() is namespace-local. `guest_pid` is the HOST-side pid from
//     spawn(). Comparing them is a category error that happened to be
//     invisible because it always said "no": measured, the guest saw itself as
//     pid 3, pgid 1, and its child as pid 4, while the supervisor was
//     comparing against a host pid in the hundreds of thousands.
//
// Observed: `timeout 1 sleep 5` ran the full 5 seconds and exited 0. The timer
// fires, the kill is refused with EPERM, and coreutils gives up silently -- so
// a hung command in the sandbox was never killed and anything using `timeout`
// as a watchdog had none.
//
// The fix is to stop guessing and ask the kernel in the REQUESTER's own terms.
// /proc/<requester>/task/<tid>/children is a host-pid list of that thread's
// direct children; walking it transitively gives the descendant set. To turn
// the guest's namespace-local target into a host pid we read NSpid out of
// /proc/<host_pid>/status, whose LAST field is the pid in the innermost
// namespace -- exactly the number the guest used.
//
// Fails CLOSED at every step: an unreadable proc file, a missing NSpid line,
// or a walk that exceeds its bound all mean "not a descendant".
//
// Bounded deliberately. The walk is a DoS surface otherwise: a guest that
// forks a deep tree would make the SUPERVISOR do unbounded work inside the
// poll loop that the guest is blocked on.
[[nodiscard]] bool is_descendant_of(std::uint32_t requester_host_pid,
                                    std::int32_t  target_ns_pid) noexcept {
    if (target_ns_pid <= 0 || requester_host_pid == 0) return false;

    constexpr int kMaxVisit = 4096;    // generous for a build, bounded for a bomb

    // Read a whole small proc file. Returns empty on any failure.
    const auto slurp = [](const char* path) -> std::string {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return {};
        std::string out;
        char buf[4096];
        for (;;) {
            const auto n = ::read(fd, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
            if (out.size() > 64 * 1024) break;     // a children list this big is a bomb
        }
        ::close(fd);
        return out;
    };

    // The pid `host_pid` has in its INNERMOST namespace, from NSpid. A kernel
    // without NSpid (pre-4.1) reports nothing, and we fail closed.
    const auto ns_pid_of = [&](std::uint32_t host_pid) -> std::int32_t {
        char path[64];
        std::snprintf(path, sizeof path, "/proc/%u/status", host_pid);
        const std::string st = slurp(path);
        const auto at = st.find("\nNSpid:");
        if (at == std::string::npos) return -1;
        const auto eol = st.find('\n', at + 1);
        const std::string line = st.substr(at + 1, eol - at - 1);
        // "NSpid:\t<outer>\t<...>\t<innermost>" -- take the LAST field.
        const auto sp = line.find_last_of(" \t");
        if (sp == std::string::npos) return -1;
        return static_cast<std::int32_t>(std::strtol(line.c_str() + sp + 1,
                                                     nullptr, 10));
    };

    // Walk the requester's descendants, breadth-first, and ask each one what
    // pid it thinks it has. Comparing in the GUEST's number space is what
    // makes this correct regardless of how many namespaces are nested.
    std::vector<std::uint32_t> frontier{requester_host_pid};
    int visited = 0;
    while (!frontier.empty() && visited < kMaxVisit) {
        const std::uint32_t pid = frontier.back();
        frontier.pop_back();
        ++visited;

        if (pid != requester_host_pid && ns_pid_of(pid) == target_ns_pid)
            return true;

        // Children are per-THREAD, so every task of this process contributes.
        char tdir[64];
        std::snprintf(tdir, sizeof tdir, "/proc/%u/task", pid);
        DIR* d = ::opendir(tdir);
        if (!d) continue;
        while (const dirent* e = ::readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char cpath[128];
            std::snprintf(cpath, sizeof cpath, "/proc/%u/task/%s/children",
                          pid, e->d_name);
            const std::string kids = slurp(cpath);
            const char* p = kids.c_str();
            while (*p) {
                while (*p == ' ' || *p == '\n') ++p;
                if (!*p) break;
                char* end = nullptr;
                const long v = std::strtol(p, &end, 10);
                if (end == p) break;
                if (v > 0) frontier.push_back(static_cast<std::uint32_t>(v));
                p = end;
            }
        }
        ::closedir(d);
    }
    return false;
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
        // ...and a DESCENDANT, which is the ordinary debugger case the
        // paragraph above describes ("a debugger attaching to its own child").
        // Equality alone could never express it: a child's pid is neither the
        // leader's pid nor the pgid.
        if (is_descendant_of(guest_pid, static_cast<std::int32_t>(target)))
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
            // ...or a group LED BY one of our descendants. `timeout` does
            // exactly this: it putpgid()s the command into its own group and
            // then signals kill(-pgid), so the pgid it passes is the child's
            // namespace-local pid. The equality tests above are both
            // host-side, so this form was always denied -- which is why
            // `timeout 1 sleep 5` still ran the full five seconds even after
            // the per-pid case started working.
            //
            // Safe for the same reason the per-pid case is: a group led by
            // our descendant contains only processes that descendant could
            // signal itself, and it cannot move a foreign process into a
            // group it leads.
            if (is_descendant_of(guest_pid, static_cast<std::int32_t>(grp)))
                return record("kill", std::move(detail), true);
            return record("kill", std::move(detail), false);
        }
        if (guest_pgid != 0 && static_cast<std::uint32_t>(target) == guest_pgid)
            return record("kill", std::move(detail), true);
        // A process in our own group -- i.e. one of our descendants. This is
        // the case the comment above promises and the equality tests could
        // not deliver: `timeout` killing the command it supervises, `make`
        // killing a failed recipe, a runner reaping its workers.
        if (is_descendant_of(guest_pid, target))
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
