// decomposition_memory_test — retrieval-augmented orchestration store.
// Verifies capture, exact + tier-fallback recall, dedup, and disk round-trip
// against an isolated temp workspace.

#include "agentty/domain/decomposition_memory.hpp"
#include "agentty/domain/routing_memory.hpp"   // turn_signature

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace agentty::smart;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main() {
    auto root = fs::temp_directory_path() /
                ("agentty_dm_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    auto& dm = DecompositionMemory::instance();
    dm.set_root_for_test(root.string());

    const std::string sig = turn_signature(Complexity::Complex, "refactor the auth module");

    // Empty recall when nothing stored.
    CHECK(dm.recall(sig).empty(), "empty store → no recall");

    // Record a decomposition, recall it exactly.
    dm.record(sig, "refactor the auth module",
              {"explorer: map auth call sites", "coder: apply the change", "tester: run auth tests"});
    auto hit = dm.recall(sig);
    CHECK(hit.size() == 1, "exact signature recalls one record");
    CHECK(hit.size() == 1 && hit[0].steps.size() == 3, "recalled steps preserved");

    // Dedup: recording the identical pattern again doesn't double it.
    dm.record(sig, "refactor the auth module",
              {"explorer: map auth call sites", "coder: apply the change", "tester: run auth tests"});
    CHECK(dm.recall(sig, 5).size() == 1, "identical pattern deduped");

    // Tier fallback: a different Complex signature still recalls a Complex plan.
    const std::string sig2 = turn_signature(Complexity::Complex, "why is startup slow? investigate");
    CHECK(sig2 != sig, "distinct turns → distinct signatures");
    auto fb = dm.recall(sig2);
    CHECK(!fb.empty(), "tier fallback recalls a same-tier plan");

    // A Simple-tier signature must NOT recall the Complex plan.
    const std::string simple = turn_signature(Complexity::Simple, "rename x to y");
    CHECK(dm.recall(simple).empty(), "different tier → no cross-tier recall");

    // Disk round-trip: reload sees the record.
    dm.set_root_for_test(root.string());   // forces reload
    CHECK(!dm.recall(sig).empty(), "record survives reload from disk");

    fs::remove_all(root);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
