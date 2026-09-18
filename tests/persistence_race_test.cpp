// SPDX-License-Identifier: Apache-2.0
//
// persistence_race_test.cpp — drive the REAL async save queue from many
// threads, under ThreadSanitizer.
//
// race_harness_test models the write-behind shape; this one exercises the
// actual code: persistence::save_thread's coalescing map, its lazily-started
// worker, thread_index_mu, and the offset/blob bookkeeping save_thread_sync
// performs. The concurrency that matters here is real — the reducer saves on
// the UI thread while the worker fsyncs, and `agentty acp` saves from several
// session workers at once.
//
// Invariants pinned:
//   • no data race (that is TSan's job; this file just supplies the pressure)
//   • every distinct thread id survives a concurrent save storm
//   • flush drains rather than truncates
//   • a save issued AFTER flush still lands (the write-behind queue restarts
//     its worker rather than silently dropping late writes — the shape that
//     left settings_cache's worker joinable and aborted at exit)

#include "agentty/io/persistence.hpp"

#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr int kThreads = 6;
constexpr int kSaves   = 25;

agentty::Thread make_thread(int id, int gen) {
    agentty::Thread t;
    t.id    = agentty::ThreadId{"race-" + std::to_string(id)};
    t.title = "gen " + std::to_string(gen);
    agentty::Message m;
    m.role = agentty::Role::User;
    m.text = "body " + std::to_string(gen);
    t.messages.push_back(std::move(m));
    return t;
}

int count_saved() {
    int n = 0;
    for (const auto& t : agentty::persistence::load_all_threads())
        if (t.id.value.starts_with("race-")) ++n;
    return n;
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() /
        ("agentty_persistence_race_" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()));
    fs::create_directories(root);
#ifdef _WIN32
    _putenv_s("AGENTTY_HOME", root.string().c_str());
#else
    ::setenv("AGENTTY_HOME", root.string().c_str(), 1);
#endif

    std::printf("=== persistence_race_test ===\n");

    // ── Concurrent save storm ────────────────────────────────────────────
    // Every thread hammers its OWN id, so coalescing is free to drop
    // intermediate generations but must never drop an id entirely.
    {
        std::barrier sync(kThreads);
        std::vector<std::thread> ts;
        std::atomic<int> threw{0};
        for (int t = 0; t < kThreads; ++t) {
            ts.emplace_back([&, t] {
                sync.arrive_and_wait();
                for (int i = 0; i < kSaves; ++i) {
                    try { agentty::persistence::save_thread(make_thread(t, i)); }
                    catch (...) { threw.fetch_add(1); }
                }
            });
        }
        for (auto& t : ts) t.join();
        agentty::persistence::flush_pending_saves();

        check(threw.load() == 0, "save_thread threw under concurrency");
        check(count_saved() == kThreads, "a thread id was lost in the storm");
        std::printf("  save storm: %d/%d ids landed\n", count_saved(), kThreads);
    }

    // ── A save AFTER flush still lands ───────────────────────────────────
    // flush_and_stop() latches `stopping`, so the write-behind queue has to
    // restart its worker for late writes rather than drop them. A late save
    // is not hypothetical: the Quit reducer issues one, and ACP sessions can
    // save after the TUI has flushed.
    {
        agentty::persistence::save_thread(make_thread(999, 0));
        agentty::persistence::flush_pending_saves();
        const bool landed = std::ranges::any_of(
            agentty::persistence::load_all_threads(),
            [](const agentty::Thread& t) { return t.id.value == "race-999"; });
        check(landed, "a save issued after flush was dropped");
        std::printf("  post-flush save: %s\n", landed ? "persisted" : "DROPPED");
    }

    std::error_code ec;
    fs::remove_all(root, ec);

    if (failures == 0) { std::printf("PASS\n"); return 0; }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}
