// cross_process_lock_test — a shared-file section excludes a second agentty
// PROCESS, waits for it with a bound, and is legal inside a lock body.
//
// Three properties, each one a bug that shipped:
//   1. a second process blocks until the holder leaves the section
//   2. a holder that keeps it (stopped with ^Z, hung NFS) doesn't freeze the
//      waiter forever: it goes on after kSharedFileWait with cross_process=false
//   3. the wait is legal while this thread holds a lower-ranked guarded lock
//      (the credential refresh path: auth_refresh lane, then the file). jaal
//      aborts a delay_for there, which is what took CI down.
//
// Fork, not threads: a second thread is the lane's job, not the file lock's.
#include "agentty/io/shared_file.hpp"
#include "agentty/util/lock_levels.hpp"
#include "agtest_cap.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#if !defined(_WIN32)
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#  define HAVE_FORK 1
#else
#  define HAVE_FORK 0
#endif

namespace fs  = std::filesystem;
namespace sf  = agentty::persistence;
namespace lv  = agentty::lock_levels;
using clock_t_ = std::chrono::steady_clock;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
int g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }           \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

long long ms_since(clock_t_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - t0).count();
}

#if HAVE_FORK
// A child that holds `target`'s section for `hold`, telling us once it has it.
pid_t hold_in_child(const fs::path& target, std::chrono::milliseconds hold) {
    int fds[2];
    if (::pipe(fds) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        static sf::LaneOf<lv::cred_file> lane;
        sf::with_shared_file(agtest::cap(), lane, target,
            [](const maya::held<lv::cred_file>&, sf::Held h, int fd, long long ms) {
                const char c = h.cross_process ? 'y' : 'n';
                (void)!::write(fd, &c, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds{ms});
                return 0;
            }, fds[1], static_cast<long long>(hold.count()));
        ::_exit(0);
    }
    ::close(fds[1]);
    char c = 0;
    while (::read(fds[0], &c, 1) < 0 && errno == EINTR) {}
    ::close(fds[0]);
    return c == 'y' ? pid : -1;
}
#endif
}  // namespace (fold)

int main() {
    std::printf("[cross_process_lock]\n");
    const fs::path target = fs::temp_directory_path() /
        ("agentty_xplock_" + std::to_string(::getpid()) + ".json");

#if HAVE_FORK
    // 1 + 3: a child holds the credential file's section for 300 ms. We wait
    // for it from INSIDE the refresh lane, as the provider refresh does.
    {
        const pid_t pid = hold_in_child(target, std::chrono::milliseconds{300});
        CHECK(pid > 0, "child entered the section");
        static maya::guarded<bool, lv::auth_refresh> refresh_lane;
        const auto t0 = clock_t_::now();
        const auto [cross, waited] = refresh_lane.with(agtest::cap(),
            [](const maya::held<lv::auth_refresh>& h, bool&, std::string t) {
                static sf::LaneOf<lv::cred_file> lane;
                const auto start = clock_t_::now();
                const bool got = sf::with_shared_file(h, lane, fs::path{t},
                    [](const maya::held<lv::cred_file>&, sf::Held held) {
                        return held.cross_process;
                    });
                return std::pair{got, ms_since(start)};
            }, target.string());
        std::printf("  info: waited %lld ms for the child's 300 ms hold\n", waited);
        CHECK(cross, "got the cross-process lock once the child left");
        CHECK(waited >= 150, "blocked while another PROCESS held the section");
        CHECK(ms_since(t0) < 3000, "and picked it up promptly after");
        int st = 0;
        ::waitpid(pid, &st, 0);
    }

    // 2: a child that never lets go. The section must give up at its bound
    // and run anyway, without the cross-process guarantee.
    {
        const pid_t pid = hold_in_child(target, std::chrono::seconds{60});
        CHECK(pid > 0, "child entered the section (and will keep it)");
        static sf::Lane lane;
        const auto t0 = clock_t_::now();
        const bool cross = sf::with_shared_file(agtest::cap(), lane, target,
            [](const maya::held<lv::file_lane>&, sf::Held held) { return held.cross_process; });
        const auto took = ms_since(t0);
        std::printf("  info: gave up after %lld ms (bound %lld ms)\n", took,
                    static_cast<long long>(sf::kSharedFileWait.count()));
        CHECK(!cross, "a stuck holder degrades to in-process, it doesn't hang");
        CHECK(took >= sf::kSharedFileWait.count() - 50 &&
              took < sf::kSharedFileWait.count() + 2000,
              "gave up at the bound");
        ::kill(pid, SIGKILL);
        int st = 0;
        ::waitpid(pid, &st, 0);
    }
#else
    std::printf("  skip: no fork on this platform (cross-process case)\n");
#endif

    std::error_code ec;
    fs::remove(target, ec);
    fs::remove(fs::path(target.string() + ".lock"), ec);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
