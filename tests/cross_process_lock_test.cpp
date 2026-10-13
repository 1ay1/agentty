// cross_process_lock_test — auth::CrossProcessFileLock mutual exclusion.
//
// The lock guards OAuth token refresh across separate agentty PROCESSES so a
// herd of instances that all see the same expired token don't each fire a
// refresh (which, with rotating refresh tokens, mutually invalidates them).
// This asserts the cross-process contract: while one holder has the lock, a
// second process BLOCKS until the first releases. Verified by forking a child
// that races for the same lock and timing when it acquires relative to the
// parent's release.
//
// On non-fork platforms the cross-process case is skipped (the lock still
// compiles and works; we just can't spawn a peer here).
#include "agentty/auth/auth.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace fs = std::filesystem;
using agentty::auth::CrossProcessFileLock;
using clock_t_ = std::chrono::steady_clock;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

}  // namespace (fold)

int main() {
    std::printf("[cross_process_lock]\n");

    fs::path base = fs::temp_directory_path() /
        ("agentty_xplock_" + std::to_string(::getpid()) + ".guard");

    // Basic: a single holder reports held(); a re-lock in the SAME process
    // via flock is re-entrant on the same fd but we use distinct instances —
    // just confirm construction/held works and the lock file is created.
    {
        CrossProcessFileLock l(base);
        CHECK(l.held(), "lock acquired (held() true)");
        CHECK(fs::exists(fs::path(base.string() + ".lock")),
              "lock file created next to the guarded path");
    }

#if HAVE_FORK
    // Cross-process: parent holds the lock for ~250ms; child races for it and
    // records how long it waited. The child must NOT acquire until the parent
    // releases (wait ≳ the parent's hold, minus scheduling slack).
    constexpr auto kHold = std::chrono::milliseconds(250);

    // A shared file the child writes its measured wait (ms) into.
    fs::path result = fs::path(base.string() + ".childwait");

    // Parent takes the lock FIRST, then forks, so the child always contends.
    auto parent_lock = std::make_unique<CrossProcessFileLock>(base);
    CHECK(parent_lock->held(), "parent holds the lock before forking");

    pid_t pid = ::fork();
    if (pid == 0) {
        // Child: race for the same lock, measure the block duration.
        auto t0 = clock_t_::now();
        CrossProcessFileLock child_lock(base);
        auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_t_::now() - t0).count();
        std::FILE* f = std::fopen(result.c_str(), "w");
        if (f) { std::fprintf(f, "%lld", static_cast<long long>(waited)); std::fclose(f); }
        _exit(child_lock.held() ? 0 : 3);
    }

    CHECK(pid > 0, "fork succeeded");
    // Hold the lock long enough that the child provably blocks on it.
    std::this_thread::sleep_for(kHold);
    parent_lock.reset();   // release — child should now acquire

    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child acquired the lock (exit 0) after parent released");

    long long child_waited = -1;
    if (std::FILE* f = std::fopen(result.c_str(), "r")) {
        if (std::fscanf(f, "%lld", &child_waited) != 1) child_waited = -1;
        std::fclose(f);
    }
    std::printf("  info: child waited %lld ms (parent held ~%lld ms)\n",
                child_waited, static_cast<long long>(kHold.count()));
    // The child must have been blocked for a substantial fraction of the hold
    // — if it acquired immediately the lock isn't cross-process. Allow slack.
    CHECK(child_waited >= kHold.count() / 2,
          "child blocked until the parent released (mutual exclusion holds)");

    std::error_code ec;
    fs::remove(result, ec);
#else
    std::printf("  skip: no fork on this platform (cross-process case)\n");
#endif

    std::error_code ec2;
    fs::remove(fs::path(base.string() + ".lock"), ec2);

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
