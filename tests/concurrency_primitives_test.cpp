// concurrency_primitives_test — proves the two BEATS-RUST primitives actually
// FIRE, not just compile:
//
//   1. util::RankedLock  — the lock-order tripwire. Acquiring a lock whose
//      rank is <= a rank already held on this thread must std::abort() in a
//      debug build. We fork a child that deliberately takes the locks out of
//      order and assert the child dies by SIGABRT.
//
//   2. util::isolated_thread / run_isolated_detached — the terminate-proof
//      worker. A body that throws must NOT propagate to std::terminate; the
//      thread swallows it (routing to dbglog) and the process survives. We
//      run a throwing body on an owned isolated_thread and assert we reach the
//      line after join() alive.
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

#include "agentty/util/isolated_thread.hpp"
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

// ── 2. Terminate-proof isolated worker ─────────────────────────────────────
// A worker body that throws must be swallowed; the process must survive and
// the thread must join cleanly.
void test_isolated_thread_swallows_throw() {
    std::atomic<bool> body_ran{false};
    {
        agentty::util::isolated_thread t("test.throwing_worker",
            [&] {
                body_ran = true;
                throw std::runtime_error("boom — must not reach std::terminate");
            });
        // Destructor joins. If the throw escaped, we'd std::terminate here and
        // never reach the CHECK below.
    }
    CHECK(body_ran.load(), "isolated worker body executed");
    CHECK(true, "throwing worker did NOT terminate the process (survived join)");
}

// The fire-and-forget variant must also isolate a throw. We give it a moment
// to run and assert the process is still alive afterward.
void test_run_isolated_detached_survives_throw() {
    std::atomic<int> counter{0};
    for (int i = 0; i < 8; ++i) {
        agentty::util::run_isolated_detached("test.detached_worker",
            [&counter] {
                counter.fetch_add(1);
                throw std::logic_error("detached boom");
            });
    }
    // Spin briefly for the detached workers to run; even if some haven't, the
    // point is that NONE terminated the process.
    for (int spin = 0; spin < 100 && counter.load() < 8; ++spin)
#if AGENTTY_HAS_FORK
        usleep(1000);
#else
        ;
#endif
    CHECK(true, "8 throwing detached workers did not terminate the process");
}

} // namespace

int main() {
    test_lock_order_legal();
    test_lock_order_tripwire();
    test_isolated_thread_swallows_throw();
    test_run_isolated_detached_survives_throw();

    if (failures == 0) {
        std::fprintf(stderr, "\nALL concurrency-primitive checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", failures);
    return 1;
}
