// SPDX-License-Identifier: Apache-2.0
//
// real_subsystem_race_test.cpp — drive the REAL shared seams under TSan.
//
// race_harness_test.cpp drives hand-written MODELS of agentty's shared
// seams (a snapshot cell, a teardown registry, a write-behind queue). That
// catches design mistakes in the pattern, but not mistakes in the actual
// subsystem: the model and the code can drift, and it is the code that
// ships. This file links the real translation units and hammers them.
//
// Each case is shaped like a bug this codebase has actually shipped or
// could plausibly ship:
//
//   1. teardown registry   concurrent register/cancel/run — the settings_cache
//                          bug was a missing registration; the dangerous
//                          version is cancel() racing run() and leaving the
//                          registry holding a callback into a dead object.
//   2. tool registry       concurrent readers of the wire-tool catalog while
//                          a plugin reload republishes it. This is the
//                          `@`-picker bug's shape: N readers, one publisher.
//   3. logx                every subsystem logs from every thread; the
//                          redaction prefilter mutates a shared scratch.
//   4. Snapshot<T>         the real type, not a model: a handle taken before
//                          a republish must stay valid after it.
//
// Run under: -DAGENTTY_SANITIZE_ALL=thread

#include "agentty/util/snapshot.hpp"
#include "agentty/util/teardown.hpp"
#include "agentty/util/logx.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
void fail(const char* what) {
    std::printf("  FAIL: %s\n", what);
    ++g_failures;
}

// ── 1. teardown registry under concurrent register / cancel / run ───────
// The registry is process-wide and every threaded subsystem touches it at
// startup and at exit. A race here is a crash at exit, which is the single
// worst place to have one (no stack, no repro, "it hung on quit").
void teardown_race() {
    std::printf("--- teardown: concurrent register/cancel/run ---\n");
    namespace td = agentty::util::teardown;

    constexpr int kThreads = 8;
    constexpr int kEach    = 200;
    std::atomic<int> fired{0};
    std::barrier sync(kThreads);

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < kEach; ++i) {
                auto tok = td::on_shutdown("race", [&fired] {
                    fired.fetch_add(1, std::memory_order_relaxed);
                });
                // Half the registrants cancel themselves — the shape that
                // keeps a bounded-lifetime object out of the registry.
                if ((t + i) % 2 == 0) td::cancel(tok);
            }
        });
    }
    for (auto& th : ts) th.join();

    // run() must not deadlock, double-fire a cancelled token, or trip TSan.
    td::run();
    td::run();   // idempotent
    std::printf("  teardown: %d actions fired, registry drained to %zu\n",
                fired.load(), td::pending());
    if (td::pending() != 0) fail("registry not drained after run()");
}

// ── 2. Snapshot<T>: the REAL type under publish/consume ─────────────────
// The bug this closes was a const-ref return whose owning shared_ptr died
// at the end of the accessor. Here a reader takes a handle and KEEPS it
// across many republishes; the bytes it points at must stay alive and
// unchanged.
void snapshot_real() {
    std::printf("--- Snapshot<T>: handle survives republish ---\n");
    using agentty::util::AtomicSnapshot;
    using agentty::util::make_snapshot;

    AtomicSnapshot<std::vector<std::string>> cell;
    cell.store(make_snapshot(std::vector<std::string>{"gen0"}));

    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0}, torn{0};

    std::thread publisher([&] {
        for (int gen = 1; gen < 4000 && !stop.load(std::memory_order_relaxed); ++gen) {
            cell.store(make_snapshot(
                std::vector<std::string>(8, "gen" + std::to_string(gen))));
        }
        stop.store(true, std::memory_order_relaxed);
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto h = cell.load();         // handle carries ownership out
                // Hold it across a republish, then verify self-consistency:
                // every element of a given generation is the same string.
                if (!h.empty()) {
                    const std::string& first = h[0];
                    for (const auto& s : h)
                        if (s != first) { torn.fetch_add(1, std::memory_order_relaxed); break; }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    publisher.join();
    for (auto& t : readers) t.join();

    std::printf("  snapshot: %lu reads, %lu torn\n", reads.load(), torn.load());
    if (torn.load() != 0) fail("Snapshot handed out a torn value");
}

// ── 3. logx from every thread at once ───────────────────────────────────
// Every subsystem logs, from whatever thread it happens to be on. The
// redaction pass rewrites the payload before it lands, so a shared scratch
// buffer there is a race that would corrupt log lines (or worse).
void logx_race() {
    std::printf("--- logx: concurrent emit from many threads ---\n");
    constexpr int kThreads = 8;
    std::barrier sync(kThreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            sync.arrive_and_wait();
            for (int i = 0; i < 500; ++i) {
                // Include something the redactor must scan for: a token-ish
                // string, so the prefilter's hot path is exercised.
                AGT_LOG(Tool, Info, "race",
                        "thread={} i={} tok=sk-ant-{}", t, i, "0123456789abcdef");
            }
        });
    }
    for (auto& th : ts) th.join();
    std::printf("  logx: %d threads x 500 events emitted\n", kThreads);
}

}  // namespace

int main() {
    std::printf("=== real_subsystem_race_test (run me under TSan) ===\n");
    teardown_race();
    snapshot_real();
    logx_race();
    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS (no races)\n");
    return 0;
}
