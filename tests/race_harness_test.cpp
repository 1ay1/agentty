// SPDX-License-Identifier: Apache-2.0
//
// race_harness_test.cpp — the THREAD-SANITIZER lane.
//
// Why this file exists: CI runs ASan + UBSan, which between them catch
// use-after-free, leaks, and UB — but NOT data races. Every concurrency bug
// this codebase has actually shipped was invisible to that pair:
//
//   • maya's BackgroundQueue destroyed by one of its own workers (a
//     shared_ptr cycle through the task payload) → self-join → abort.
//   • settings_cache's worker left joinable in a function-local static →
//     ~std::thread → terminate at exit.
//   • the `@` picker spawning a thread per keystroke, N of them racing to
//     publish into one git-status map, last writer winning arbitrarily.
//
// The first two were found by hand-written probes and the third by reading.
// That is not a process. This file is the process: it drives each shared
// seam from many threads at once so TSan can see the interleavings, and it
// is registered under the `sanitizer` label so CI runs it.
//
// These cases are deliberately SHAPED like the bugs above — publish/consume
// races, teardown-with-backlog, concurrent readers of a snapshot being
// republished — rather than being generic smoke tests.

#include "agentty/util/snapshot.hpp"
#include "agentty/util/teardown.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using agentty::util::Snapshot;
using agentty::util::make_snapshot;
namespace teardown = agentty::util::teardown;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// Threads that all start at once see far more interleavings than threads
// that trickle in, so every case below rendezvouses on a barrier first.
constexpr int kThreads = 8;
constexpr int kIters   = 2000;

// ── 1. AtomicSnapshot: readers vs. a republishing writer ─────────────────
//
// THE use-after-free shape. list_workspace_symbols() used to hand out a
// const-ref to a buffer whose owning shared_ptr had already died; readers
// then walked freed memory while a background walk republished.
//
// Note this drives AtomicSnapshot, not a bare Snapshot. A bare Snapshot has
// shared_ptr semantics — copying the HANDLE while another thread assigns to
// that same handle is a plain data race on the pointer, which TSan correctly
// reports (that is how this distinction was found). AtomicSnapshot is the
// type for a producer/consumer slot, and it is what the caches use.
void snapshot_publish_race() {
    agentty::util::AtomicSnapshot<std::vector<std::string>> shared;
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};

    // Producer: republish continuously.
    std::thread producer([&] {
        for (int gen = 0; !stop.load(std::memory_order_relaxed); ++gen) {
            std::vector<std::string> v;
            v.reserve(64);
            for (int i = 0; i < 64; ++i)
                v.push_back("gen" + std::to_string(gen) + "_" + std::to_string(i));
            shared.store(std::move(v));
        }
    });

    // Readers: take a handle, then walk it. The walk must stay valid for the
    // whole traversal even though the producer is swapping underneath.
    std::barrier sync(kThreads);
    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&] {
            sync.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                const auto local = shared.load();   // one atomic refcount bump
                std::size_t n = 0;
                for (const auto& s : local) n += s.size();
                // Either unpublished (empty) or a complete generation —
                // never a half-swapped buffer.
                check(local.empty() || local.size() == 64,
                      "snapshot reader saw a torn buffer");
                reads.fetch_add(static_cast<long>(n != 0), std::memory_order_relaxed);
            }
        });
    }
    for (auto& r : readers) r.join();
    stop.store(true);
    producer.join();
    std::printf("  snapshot publish/consume: %ld non-empty reads\n", reads.load());
}

// ── 2. Snapshot: the handle outlives every republish ─────────────────────
//
// Directly the bug: hold a reader handle, let the producer swap many times,
// and assert the ORIGINAL contents are still intact and readable. Under the
// old const-ref contract this is a read of freed memory.
void snapshot_handle_outlives_producer() {
    auto held = make_snapshot(std::vector<std::string>{"alpha", "beta", "gamma"});

    agentty::util::AtomicSnapshot<std::vector<std::string>> latest;
    latest.store(held);
    std::thread churn([&] {
        for (int i = 0; i < 500; ++i)
            latest.store(std::vector<std::string>(32, "x" + std::to_string(i)));
    });
    churn.join();

    check(held.size() == 3,        "held snapshot changed size");
    check(held[0] == "alpha",      "held snapshot corrupted (0)");
    check(held[2] == "gamma",      "held snapshot corrupted (2)");
}

// ── 3. teardown registry: concurrent registration + drain ────────────────
//
// The registry is written from whichever thread first starts a subsystem's
// worker and drained from main()'s scope guard, so registration races the
// drain by construction.
void teardown_registration_race() {
    teardown::run();                       // clean slate

    std::atomic<int> fired{0};
    std::barrier sync(kThreads);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            sync.arrive_and_wait();
            for (int i = 0; i < 200; ++i) {
                const auto tok = teardown::on_shutdown(
                    "racer", [&] { fired.fetch_add(1, std::memory_order_relaxed); });
                // Half withdraw, half leave it for the drain — exercises the
                // cancel/run interleaving that a scoped registrant hits.
                if (i % 2 == 0) teardown::cancel(tok);
            }
        });
    }
    // Drain concurrently with the registrations above.
    for (int i = 0; i < 50; ++i) {
        teardown::run();
        std::this_thread::yield();
    }
    for (auto& t : ts) t.join();
    teardown::run();                       // final drain

    check(teardown::pending() == 0, "teardown registry not drained");
    std::printf("  teardown registry: %d actions fired\n", fired.load());
}

} // namespace

int main() {
    std::printf("=== race_harness_test (run me under TSan) ===\n");

    std::printf("--- snapshot publish/consume race ---\n");
    snapshot_publish_race();

    std::printf("--- snapshot handle outlives producer ---\n");
    snapshot_handle_outlives_producer();

    std::printf("--- teardown registration race ---\n");
    teardown_registration_race();

    if (failures == 0) {
        std::printf("PASS (no races, no torn reads)\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}
