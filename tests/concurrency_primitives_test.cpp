// concurrency_primitives_test — proves the two BEATS-RUST primitives actually
// FIRE, not just compile:
//
//   1. util::RankedLock  — the lock-order tripwire. Acquiring a lock whose
//      rank is <= a rank already held on this thread must std::abort() in a
//      debug build. We fork a child that deliberately takes the locks out of
//      order and assert the child dies by SIGABRT.
//
//   2. util::run_isolated_detached — background work that is OWNED. Three
//      guarantees, and only the first one used to hold:
//        a. a body that throws does NOT reach std::terminate,
//        b. the work is WAITED FOR at shutdown, not detached and forgotten,
//        c. the body is handed a stop_token and asked to stop.
//      (b) and (c) arrived with jaal::kernel::pool. The old hand-rolled
//      version was a plain std::thread(...).detach() whose comment claimed a
//      "self-joining reaper" that did not exist, so there was nothing to
//      join and nothing to cancel. The test below would have failed it.
//
// The abort path is exercised in a forked child (like cred_crypt_test) so the
// deliberate std::abort() doesn't take down the whole ctest process.
//
// NOTE: the tripwire's runtime check is compiled only in debug builds
// (#ifndef NDEBUG in ranked_lock.hpp). When the test itself is built with
// NDEBUG the abort won't fire — so that half auto-passes with a clear note.

#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  define AGENTTY_HAS_FORK 0
#else
#  define AGENTTY_HAS_FORK 1
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "agentty/util/background.hpp"
#include "agentty/util/ranked_lock.hpp"

namespace {

int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
        else         { std::fprintf(stderr, "ok:   %s\n", (msg)); }             \
    } while (0)

// ── 1. Ranked-lock tripwire ────────────────────────────────────────────────
// In the child: take a HIGH rank first, then a LOW rank nested inside it. That
// is the out-of-order (potential ABBA) acquisition the tripwire must catch.
[[noreturn]] void child_trips_lock_order() {
    agentty::util::RankedMutex<20> inner;   // higher rank (would be the INNER lock)
    agentty::util::RankedMutex<10> outer;   // lower rank  (should be taken FIRST)

    agentty::util::RankedLock hi(inner);    // hold rank 20
    // Now take rank 10 while holding 20 — strictly-decreasing => tripwire.
    agentty::util::RankedLock lo(outer);    // must std::abort() here (debug)

    // If we get here the tripwire did NOT fire. Exit 0 so the parent sees the
    // "did not abort" outcome distinctly from the SIGABRT it expects.
    std::_Exit(0);
}

void test_lock_order_tripwire() {
#ifdef NDEBUG
    std::fprintf(stderr,
        "skip: lock-order tripwire is a debug-only runtime check (NDEBUG "
        "build) — compile the test without NDEBUG to exercise it.\n");
    return;
#elif !AGENTTY_HAS_FORK
    std::fprintf(stderr,
        "skip: lock-order tripwire abort path needs fork() (POSIX only).\n");
    return;
#else
    pid_t pid = fork();
    if (pid == 0) {
        // Child: trip the tripwire (it std::abort()s).
        child_trips_lock_order();
        _exit(0);
    }
    CHECK(pid > 0, "fork for lock-order child succeeded");
    int status = 0;
    waitpid(pid, &status, 0);
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    CHECK(aborted,
          "out-of-order lock acquisition abort()s (tripwire fired)");
#endif
}

// A correctly-ordered nesting (low rank OUTER, high rank INNER) must NOT
// abort — proves the tripwire doesn't false-positive on the legal order.
void test_lock_order_legal() {
    agentty::util::RankedMutex<10> outer;
    agentty::util::RankedMutex<20> inner;
    bool reached = false;
    {
        agentty::util::RankedLock lo(outer);   // rank 10 first (outer)
        agentty::util::RankedLock hi(inner);   // rank 20 nested (inner) — legal
        reached = true;
    }
    CHECK(reached, "legal nesting (10 then 20) does not abort");
}

// ── 2. Background work that is OWNED ───────────────────────────────────────
// One test, one shutdown: the pool's shutdown is terminal (later posts are
// dropped), so isolation and cancellation are asserted against the same
// drain rather than in two passes.
//
// The spin-and-hope the old version ended with ("even if some haven't run,
// the point is none terminated") is exactly what a detached thread forces on
// a test. Here shutdown() JOINS, so the counter is an assertion and not a
// race: if the work were still detached, these counts would be unreliable
// and `stuck` would be meaningless.
void test_background_work_is_owned() {
    std::atomic<int>  ran{0};
    std::atomic<bool> saw_stop{false};

    // (a) throwing bodies must be isolated, every one of them.
    for (int i = 0; i < 8; ++i) {
        agentty::util::run_isolated_detached("test.throwing_worker",
            [&ran] {
                ran.fetch_add(1);
                throw std::logic_error("boom — must not reach std::terminate");
            });
    }

    // (c) a body that asks for the stop_token must actually be stopped.
    agentty::util::run_isolated_detached("test.cancellable_worker",
        [&saw_stop](std::stop_token st) {
            while (!st.stop_requested()) {
#if AGENTTY_HAS_FORK
                usleep(500);
#endif
            }
            saw_stop = true;
        });

    // (b) shutdown REQUESTS STOP and WAITS. Returns how many were still
    // stuck at the grace deadline.
    const std::size_t stuck = agentty::util::background_pool().shutdown();

    CHECK(ran.load() == 8,
          "every throwing body ran and was waited for (not detached)");
    CHECK(saw_stop.load(),
          "a background body is handed a stop_token and is asked to stop");
    CHECK(stuck == 0, "shutdown drained every job within the grace");
    CHECK(true, "8 throwing workers did not terminate the process");
}

} // namespace

int main() {
    test_lock_order_legal();
    test_lock_order_tripwire();
    test_background_work_is_owned();

    if (failures == 0) {
        std::fprintf(stderr, "\nALL concurrency-primitive checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", failures);
    return 1;
}
