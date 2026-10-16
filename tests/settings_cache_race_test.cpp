// SPDX-License-Identifier: Apache-2.0
//
// settings_cache_race_test.cpp — the real settings write-behind cache under
// TSan, with an in-memory "disk".
//
//   1. many threads load, save and invalidate at once: no race, no crash,
//      and after a final flush the disk holds the last value saved.
//   2. a disk load that is still in flight when invalidate() announces an
//      outside write must not cache the old file over it.
//
// Run under: -DAGENTTY_SANITIZE_ALL=thread

#include "agentty/runtime/app/settings_cache.hpp"
#include "agentty/util/teardown.hpp"
#include "agtest_cap.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sc = agentty::app::settings_cache;
using agentty::store::Settings;

// settings_cache registers itself for outside-write notices; the test calls
// invalidate() directly, so the hook is a no-op here.
namespace agentty::persistence {
void on_settings_written(std::function<void(const maya::unlocked&)>) {}
}

namespace {

int g_failures = 0;
void fail(const char* what) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
}

// The fake disk: one value, guarded, plus a hook to stall a load mid-read.
struct Disk {
    maya::guarded<std::string, maya::leaf> value{std::string{"v0"}};
    std::atomic<bool> stall_next_load{false};
    std::atomic<bool> load_started{false};
    std::atomic<bool> release_load{false};
};
Disk g_disk;

Settings settings_with(const std::string& id) {
    Settings s;
    s.model_id = agentty::ModelId{id};
    return s;
}

Settings disk_load() {
    auto v = g_disk.value.read(agtest::cap(), [](const std::string& s) { return s; });
    if (g_disk.stall_next_load.exchange(false)) {
        g_disk.load_started = true;
        while (!g_disk.release_load.load()) std::this_thread::yield();
    }
    return settings_with(v);
}

void disk_save(const maya::unlocked& cap, const Settings& s) {
    g_disk.value.with(cap, [](std::string& d, std::string v) { d = std::move(v); },
                      s.model_id.value);
}

std::string disk_now() {
    return g_disk.value.read(agtest::cap(), [](const std::string& s) { return s; });
}

void hammer(const sc::Seam& seam) {
    std::printf("--- settings cache: concurrent load/save/invalidate ---\n");
    constexpr int kThreads = 8;
    constexpr int kEach    = 200;
    std::barrier sync(kThreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < kEach; ++i) {
                switch ((t + i) % 4) {
                    case 0: (void)seam.load(agtest::cap()); break;
                    case 1: seam.save(agtest::cap(),
                                      settings_with("t" + std::to_string(t))); break;
                    case 2: (void)seam.load(agtest::cap()); break;
                    case 3: if (i % 16 == 3) sc::invalidate(agtest::cap()); break;
                }
            }
        });
    }
    for (auto& th : ts) th.join();
    seam.save(agtest::cap(), settings_with("final"));
    sc::flush(agtest::cap());
    if (disk_now() != "final") fail("last save didn't reach disk");
    if (seam.load(agtest::cap()).model_id.value != "final")
        fail("cache doesn't hold the last save");
}

void stale_load_vs_invalidate(const sc::Seam& seam) {
    std::printf("--- settings cache: a load racing an invalidate ---\n");
    sc::invalidate(agtest::cap());   // empty the cache
    g_disk.value.with(agtest::cap(), [](std::string& d) { d = "old"; });

    // A load reads "old", then stalls before publishing.
    g_disk.stall_next_load = true;
    std::thread loader([&] { (void)seam.load(agtest::cap()); });
    while (!g_disk.load_started.load()) std::this_thread::yield();

    // Meanwhile another process writes the file and we're told about it.
    g_disk.value.with(agtest::cap(), [](std::string& d) { d = "new"; });
    sc::invalidate(agtest::cap());

    g_disk.release_load = true;
    loader.join();

    if (seam.load(agtest::cap()).model_id.value != "new")
        fail("a load that started before invalidate() cached the old file");
}

}  // namespace

int main() {
    std::printf("=== settings_cache_race_test (run me under TSan) ===\n");
    auto seam = sc::wrap(agtest::cap(), disk_load, disk_save);
    hammer(seam);
    stale_load_vs_invalidate(seam);
    sc::shutdown(agtest::cap());
    agentty::util::teardown::run(agtest::cap());
    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
