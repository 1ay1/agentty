// child_process_race_test — ChildProcess under concurrent stop/close/write.
//
// The child's stdin fd and the process itself are reached from several
// threads in real use: the peer's writer writes, stop() closes stdin, the
// owner's destructor terminates and reaps. This hammers those together so
// TSan (build-tsan) sees any unsynchronised access, and the plain build
// checks nothing deadlocks or crashes.
#include "agentty/util/child_process.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if AGENTTY_HAVE_CHILD_PROCESS

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

agentty::util::ChildProcess::Spawn cat_spawn() {
    agentty::util::ChildProcess::Spawn s;
#if defined(_WIN32)
    s.command = "cmd";   // `more` echoes stdin and exits on EOF, like cat
    s.args    = {"/c", "more"};
#else
    s.command = "cat";   // echoes stdin; exits on EOF
#endif
    return s;
}

// The writer races close_stdin(): no write may land on a closed or reused
// fd. One writer, as in real use (the peer's writer thread); a std::ostream
// isn't safe for two.
void writers_vs_close() {
    for (int round = 0; round < 100; ++round) {
        agentty::util::ChildProcess c{cat_spawn()};
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        ts.emplace_back([&] {
            while (!go) std::this_thread::yield();
            for (int k = 0; k < 100; ++k) { c.in() << "line\n"; c.in().flush(); }
        });
        ts.emplace_back([&] {
            while (!go) std::this_thread::yield();
            c.close_stdin();
        });
        go = true;
        for (auto& t : ts) t.join();
    }
    check(true, "a writer racing close_stdin finished");
}

// alive() polls and terminate() reaps from different threads: exactly one
// reap, and the exit code is seen by both.
void poll_vs_terminate() {
    for (int round = 0; round < 50; ++round) {
        agentty::util::ChildProcess c{cat_spawn()};
        std::atomic<bool> stop{false};
        std::thread poller([&] { while (!stop) (void)c.alive(); });
        c.terminate();
        stop = true;
        poller.join();
        check(!c.alive(), "child is gone after terminate");
        check(c.exit_code().has_value(), "exit code recorded");
    }
}

// The child exits by itself (stdin EOF) while another thread terminates.
void natural_exit_vs_terminate() {
    for (int round = 0; round < 50; ++round) {
        agentty::util::ChildProcess c{cat_spawn()};
        std::thread closer([&] { c.close_stdin(); });
        std::thread killer([&] { c.terminate(); });
        closer.join();
        killer.join();
        check(!c.alive(), "child gone after a racing exit and terminate");
    }
}
}  // namespace

int main() {
    writers_vs_close();
    poll_vs_terminate();
    natural_exit_vs_terminate();
    if (failures) return 1;
    std::printf("child_process_race_test: ok\n");
    return 0;
}

#else
int main() { return 0; }
#endif
