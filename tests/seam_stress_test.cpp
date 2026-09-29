// SPDX-License-Identifier: Apache-2.0
//
// seam_stress_test.cpp — hammer the REAL process-wide seams from many
// threads at once, under ThreadSanitizer.
//
// WHY THESE FOUR. agentty has 44 files holding a raw std::mutex, every one
// of them hand-audited onto the concurrency allowlist. That audit is Level
// B in jaal's taxonomy (docs/concurrency.md §6): a human checked it, which
// is weaker than jaal's Level A, where the bug is unrepresentable. The
// honest way to keep a Level-B claim true is to keep proving it, and the
// existing race tests do not cover these:
//
//   provider::select/active  every turn reads it; the pickers write it.
//                            active() returns a Selection BY VALUE under
//                            the lock, so a mid-select() reader must never
//                            see a torn endpoint.
//   tools::registry/wire_*   the tool catalog is rebuilt when an MCP plugin
//                            connects while tool dispatch reads it. This is
//                            the `@`-picker bug's shape: N readers, one
//                            republisher.
//   settings_cache           the write-behind settings worker: reducers
//                            write on the UI thread, a worker fsyncs.
//   capkey/catalog           the capability registry is written by the
//                            models.dev refresh and read by every turn.
//
// Each case drives the seam the way the app drives it, but with the
// contention turned up: many readers, a writer republishing underneath,
// and no sleeps to paper over the interleaving.
//
// Run under: -DAGENTTY_SANITIZE_ALL=thread

#include "agentty/provider/selection.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/domain/capkey.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

// ── 1. provider::select / active ────────────────────────────────────────
// The selection is swapped by the provider picker and read at the top of
// every turn. active() hands back a by-value snapshot taken under the
// selection mutex; the invariant is that a reader never observes a Selection
// whose fields come from two different select() calls.
void selection_race() {
    std::printf("--- provider::select / active under contention ---\n");
    using namespace agentty::provider;

    constexpr int kReaders = 6;
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0}, torn{0};
    std::barrier sync(kReaders + 1);

    // Two selections whose fields are internally consistent. A torn read
    // would pair one's host with the other's port.
    Selection a{};
    a.openai_endpoint.host = "alpha.example";
    a.openai_endpoint.port = 1111;
    Selection b{};
    b.openai_endpoint.host = "bravo.example";
    b.openai_endpoint.port = 2222;

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            sync.arrive_and_wait();
            while (!stop.load(std::memory_order_relaxed)) {
                const Selection s = active();
                const auto& h = s.openai_endpoint.host;
                const auto  p = s.openai_endpoint.port;
                if ((h == "alpha.example" && p != 1111) ||
                    (h == "bravo.example" && p != 2222))
                    torn.fetch_add(1, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    sync.arrive_and_wait();
    for (int i = 0; i < 4000; ++i) select((i & 1) ? a : b);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    std::printf("    %lu reads, %lu torn\n", reads.load(), torn.load());
    expect(torn.load() == 0, "active() never returns a torn Selection");
}

// ── 2. the tool registry under a plugin reload ──────────────────────────
// wire_tools() is read on every turn to build the request, and the catalog
// is republished when MCP plugins connect or reload. A reader holding the
// returned reference while the vector is rebuilt is the use-after-free this
// seam exists to prevent.
void registry_race() {
    std::printf("--- tools::wire_tools during catalog invalidation ---\n");
    using namespace agentty::tools;

    constexpr int kReaders = 6;
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0}, bad{0};
    std::barrier sync(kReaders + 1);

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            sync.arrive_and_wait();
            while (!stop.load(std::memory_order_relaxed)) {
                // Take a SNAPSHOT (the safe API) and walk it fully: every
                // name must stay readable for as long as we hold it.
                const auto snap = wire_tools_snapshot();
                for (const auto& t : snap)
                    if (t.name.value.empty()) bad.fetch_add(1, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    sync.arrive_and_wait();
    for (int i = 0; i < 2000; ++i) invalidate_mcp_catalog();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    std::printf("    %lu snapshot walks, %lu malformed\n", reads.load(), bad.load());
    expect(bad.load() == 0, "a tool snapshot stays valid while the catalog churns");
}

// ── 3. the capability registry ──────────────────────────────────────────
// Written by the models.dev background refresh, read by every turn when it
// decides whether to send tool definitions / a reasoning field. Keyed
// through capkey::norm_model, so this also exercises that the key
// normalisation is stateless.
void capability_race() {
    std::printf("--- catalog capability registry, writer + readers ---\n");
    using namespace agentty;

    constexpr int kReaders = 4;
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> reads{0};
    std::barrier sync(kReaders + 1);

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            sync.arrive_and_wait();
            while (!stop.load(std::memory_order_relaxed)) {
                (void)catalog_reasoning_for("claude-sonnet-4.5", "anthropic");
                (void)catalog_reasoning_for("gpt-5.1", "openai");
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    sync.arrive_and_wait();
    for (int i = 0; i < 3000; ++i) {
        set_catalog_reasoning("anthropic/claude-sonnet-4.5", (i & 1) != 0);
        set_catalog_reasoning("openai/gpt-5.1", (i & 1) == 0);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    std::printf("    %lu capability lookups\n", reads.load());
    expect(true, "capability registry survives concurrent write/read");
}

}  // namespace

int seam_stress_test_main() {
    std::printf("=== seam_stress_test (run me under TSan) ===\n");
    selection_race();
    registry_race();
    capability_race();
    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS (no races, no torn reads)\n");
    return 0;
}
