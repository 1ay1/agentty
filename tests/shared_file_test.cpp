// shared_file_test — persistence::SharedFile excludes a second agentty
// PROCESS, and the agentty side of the lock is actually wired to jaal.
//
// WHAT THIS TESTS, AND WHAT IT DELIBERATELY DOES NOT CLAIM
//
// jaal's file_lock conformance suite owns the kernel semantics (a second
// process waits, a dead holder strands nothing, shared vs exclusive). This
// test owns the question jaal cannot answer: is AGENTTY's critical section
// really taking that lock, on the sidecar for the file it names, or has the
// policy layer been wired to nothing?
//
// It does NOT assert "a thread cannot vanish from the picker". It was
// written to, and that claim is false: load_all_threads() enumerates the
// THREADS DIRECTORY and uses index.json only as a metadata cache, so a lost
// index entry costs one re-parse and then self-heals. The honest cost of the
// index race is a slow load and a briefly stale title. The guarantee worth
// testing is the one below.
//
// Fork, not threads. A threaded version passes under the in-process mutex
// alone and would prove nothing about the half that was missing.

#include "agentty/io/shared_file.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#if !defined(_WIN32)
#  include <cerrno>
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace fs = std::filesystem;
namespace sf = agentty::persistence;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

sf::Lane& test_mu() {
    static sf::Lane m;
    return m;
}

}  // namespace (fold)

// The fold build renames this to shared_file_test_main via -Dmain=...; it
// gets its own process, which this test needs because it forks.
int main() {
    std::printf("shared_file_test\n");

    const fs::path dir =
        fs::temp_directory_path()
        / ("agentty_sharedfile_" + std::to_string(
#if HAVE_FORK
               ::getpid()
#else
               0
#endif
           ));
    fs::create_directories(dir);
    const fs::path target = dir / "guarded.json";

    // The lock must be real, not a silently-degraded no-op. A tmpfs/ext4
    // /tmp supports locks, so a false here means the policy layer failed to
    // reach jaal at all — which is the wiring bug this test exists to catch.
    CHECK(sf::with_shared_file(test_mu(), target, [](sf::Held h) { return h.cross_process; }),
          "with_shared_file took the cross-process lock (jaal is wired in)");

    // The lock lives on the SIDECAR, never on the target. This is the whole
    // correctness argument: agentty writes these files by renaming a new
    // inode over the target, so a lock held on the target would guard a file
    // that is no longer the file.
    {
        const auto [sidecar, touched] = sf::with_shared_file(test_mu(), target,
            [](sf::Held, std::string t) {
                return std::pair{fs::exists(fs::path(t + ".lock")), fs::exists(fs::path(t))};
            }, target.string());
        CHECK(sidecar, "the lock is held on <target>.lock");
        CHECK(!touched, "locking does not create or touch the target itself");
    }

#if HAVE_FORK
    // A second PROCESS must be excluded while we hold it. The child reports
    // how long it waited; it must have been blocked for a real fraction of
    // the parent's hold, or the exclusion is not crossing the process
    // boundary and the race is still open.
    //
    // ORDER MATTERS: the parent takes the lock BEFORE forking. Forking first
    // and acquiring after is a race the test loses under parallel load — the
    // child got in first and reported "waited 0 ms", which looks exactly
    // like a broken lock. (It failed that way under ctest -j12 while passing
    // standalone, which is the signature of a racy test, not a racy lock.)
    //
    // fcntl locks are NOT inherited across fork, so the child genuinely
    // contends rather than sharing our hold. The child uses its own mutex
    // because our copy of test_mu() is inherited LOCKED.
    {
        // The whole parent side runs inside the critical section: fork,
        // hold for kHold, then leave, and read the child's verdict after.
        const auto child_waited = sf::with_shared_file(test_mu(), target,
            [](sf::Held, std::string t) -> long long {
                constexpr auto kHold = std::chrono::milliseconds{400};
                int pipefd[2] = {-1, -1};
                if (::pipe(pipefd) != 0) return -2;
                const pid_t pid = ::fork();
                if (pid == 0) {
                    // The child continues running C++ under our lane.
                    maya::forget_held_after_fork();
                    ::close(pipefd[0]);
                    const auto t0 = std::chrono::steady_clock::now();
                    // Our copy of the lane is inherited locked; the child
                    // needs its own.
                    static sf::Lane child_lane;
                    sf::with_shared_file(child_lane, fs::path{t}, [](sf::Held) {});
                    long long v = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    (void)!::write(pipefd[1], &v, sizeof(v));
                    ::close(pipefd[1]);
                    ::_exit(0);
                }
                ::close(pipefd[1]);
                ::usleep(static_cast<unsigned>(kHold.count()) * 1000u);
                // Leaving the body releases; the child reads after. Hand the
                // pipe and pid out so the verdict is read unlocked.
                return (static_cast<long long>(pid) << 20) | pipefd[0];
            }, target.string());
        if (child_waited == -2) {
            CHECK(false, "pipe for the child's verdict");
        } else {
            const int rd = static_cast<int>(child_waited & 0xFFFFF);
            const pid_t pid = static_cast<pid_t>(child_waited >> 20);
            long long waited = -1;
            (void)!::read(rd, &waited, sizeof(waited));
            ::close(rd);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            std::printf("  info: child waited %lld ms (parent held ~400 ms)\n", waited);
            CHECK(waited >= 200, "a second PROCESS blocked until we released");
        }
    }
#else
    std::printf("  skip: no fork on this platform (cross-process case)\n");
#endif

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
