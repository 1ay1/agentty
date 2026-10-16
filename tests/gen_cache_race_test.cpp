// SPDX-License-Identifier: Apache-2.0
//
// gen_cache_race_test.cpp — util::GenCache, the model-list cache Copilot and
// Kimi use.
//
//   1. a fetch that started before clear() can't store its stale list.
//   2. many fetchers and clearers at once: no race, and a value only ever
//      lands if no clear() ran after its fetch began.
//
// Run under: -DAGENTTY_SANITIZE_ALL=thread

#include "agentty/util/gen_cache.hpp"

#include <atomic>
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
void fail(const char* what) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
}

void stale_store_dropped() {
    std::printf("--- gen_cache: a fetch from before clear() is dropped ---\n");
    agentty::util::GenCache<std::vector<int>> c;
    const auto seen = c.gen();          // fetch starts
    c.clear();                          // invalidation lands mid-fetch
    if (c.store_if({1, 2, 3}, seen)) fail("stale store reported as landed");
    if (!c.get().empty()) fail("stale list overwrote the clear");

    const auto fresh = c.gen();         // a fetch after the clear
    if (!c.store_if({4}, fresh)) fail("fresh store was dropped");
    if (c.get() != std::vector<int>{4}) fail("fresh list didn't land");
}

void hammer() {
    std::printf("--- gen_cache: concurrent fetchers and clearers ---\n");
    agentty::util::GenCache<std::vector<int>> c;
    constexpr int kThreads = 8;
    constexpr int kEach    = 2000;
    std::barrier sync(kThreads);
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < kEach; ++i) {
                if (t % 4 == 0 && i % 8 == 0) { c.clear(); continue; }
                const auto seen = c.gen();
                const bool landed = c.store_if({t, i}, seen);
                // A store that landed was made under its own generation, so
                // the generation can't have moved backwards past it.
                if (landed && c.gen() < seen)
                    bad.fetch_add(1, std::memory_order_relaxed);
                (void)c.get();
            }
        });
    }
    for (auto& th : ts) th.join();
    if (bad.load()) fail("generation went backwards");
}

}  // namespace

int main() {
    std::printf("=== gen_cache_race_test (run me under TSan) ===\n");
    stale_store_dropped();
    hammer();
    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
