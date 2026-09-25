// save_bench <thread-id> [rounds] — per-round save cost on a real thread.
//
// Loads a thread from the user's store into a scratch AGENTTY_HOME, then
// simulates an agent loop: each round appends one message and calls
// save_thread + flush. Round 1 is the full write (first save of the
// process); later rounds take the incremental tail path. Prints both so a
// regression back to whole-thread saves is obvious.
//
// Build: part of agentty_standalone_tests (LABELS perf).
// Run:   agentty_standalone_tests save_bench 1c6fff4258e07787 10

#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: save_bench <thread-file.json|.jsonl> [rounds]\n");
        return 2;
    }
    const fs::path src{argv[1]};
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 8;

    // The test main sandboxes AGENTTY_HOME, so take the source by path
    // (read-only) and let every write land in the sandbox.
    std::optional<Thread> loaded;
    if (src.extension() == ".jsonl") {
        if (auto log = ThreadLog::open_path(src)) loaded = log->load_thread();
    } else if (auto t = persistence::load_thread_file(src)) {
        loaded = std::move(*t);
    }
    if (!loaded || loaded->messages.empty()) {
        std::fprintf(stderr, "save_bench: can't load %s\n", src.string().c_str());
        return 1;
    }
    Thread t = std::move(*loaded);
    t.id = ThreadId{"save-bench-" + src.stem().string()};

    std::printf("%s: %zu messages\n", src.filename().string().c_str(), t.messages.size());
    for (int r = 0; r < rounds; ++r) {
        Message m;
        m.role = (r % 2 == 0) ? Role::User : Role::Assistant;
        m.text = "bench round " + std::to_string(r);
        t.messages.push_back(std::move(m));

        const auto a = clk::now();
        persistence::save_thread(t);            // reducer-side cost
        const auto b = clk::now();
        persistence::flush_pending_saves();     // + writer-side cost
        const auto c = clk::now();
        auto ms = [](auto d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        std::printf("round %2d  %-4s  enqueue %8.2f ms   total %9.2f ms\n",
                    r, r == 0 ? "full" : "tail", ms(b - a), ms(c - a));
    }
    persistence::delete_thread(t.id);
    return 0;
}
