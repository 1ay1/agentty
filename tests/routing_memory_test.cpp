// routing_memory_test — the per-workspace learned routing prior.
// Verifies: neutral when unseen, a sustained regret signal moves the prior in
// the right direction, a single event doesn't swing it, and it round-trips
// through the on-disk TSV. Runs against an isolated temp workspace.

#include "agentty/domain/routing_memory.hpp"

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace agentty::smart;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main() {
    // Isolated workspace so we never touch a real .agentty/.
    auto root = fs::temp_directory_path() /
                ("agentty_rm_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    auto& rm = RoutingMemory::instance();
    rm.set_root_for_test(root.string());

    // Signature is coarse + stable.
    const std::string sig = turn_signature(Complexity::Simple, "fix the auth bug");
    CHECK(!sig.empty(), "signature is non-empty");
    CHECK(turn_signature(Complexity::Simple, "fix the auth bug") == sig,
          "signature is deterministic");

    // Unseen → neutral.
    CHECK(rm.prior_bias(sig) == 0, "unseen signature → neutral prior");

    // A single regret event should NOT swing a confident prior (low evidence).
    rm.note_routed(sig);
    rm.note_regret(sig, +1);
    CHECK(rm.prior_bias(sig) == 0, "one event → still neutral (needs evidence)");

    // A SUSTAINED under-rating signal moves the prior up to +1.
    for (int i = 0; i < 12; ++i) { rm.note_routed(sig); rm.note_regret(sig, +1); }
    CHECK(rm.prior_bias(sig) == 1, "sustained under-rating → +1 prior");

    // Persistence: a fresh instance view (reload) sees the same prior.
    rm.set_root_for_test(root.string());   // forces reload from disk
    CHECK(rm.prior_bias(sig) == 1, "prior survives reload from disk");

    // A different signature is unaffected.
    const std::string other = turn_signature(Complexity::Complex, "why does it hang?");
    CHECK(other != sig, "distinct turns → distinct signatures");
    CHECK(rm.prior_bias(other) == 0, "unrelated signature stays neutral");

    // ── Integration: the loop CLOSES — sustained over-rating pulls the prior
    //    the other way, and reset wipes it. This is the end-to-end contract
    //    the finalize_turn / submit_message hooks depend on.
    const std::string ov = turn_signature(Complexity::Complex, "trivial-looking but rated complex");
    for (int i = 0; i < 12; ++i) { rm.note_routed(ov); rm.note_regret(ov, -1); }
    CHECK(rm.prior_bias(ov) == -1, "sustained over-rating → -1 prior (loop closes both ways)");

    // Telemetry count reflects the distinct signatures seen.
    CHECK(rm.learned_count() >= 2, "learned_count reflects seen signatures");

    // Intent axis: same tier + shape but different leading verb must NOT
    // collide, so a 'fix' prior doesn't bleed into an 'add' turn.
    {
        const std::string fix = turn_signature(Complexity::Standard, "fix the parser");
        const std::string add = turn_signature(Complexity::Standard, "add a parser");
        const std::string exp = turn_signature(Complexity::Standard, "explain the parser");
        CHECK(fix != add && add != exp && fix != exp,
              "distinct intents (fix/add/explain) → distinct signatures");
    }

    // Reset wipes the workspace's learning.
    rm.reset();
    CHECK(rm.prior_bias(sig) == 0 && rm.prior_bias(ov) == 0,
          "reset() clears every learned prior");
    CHECK(rm.learned_count() == 0, "reset() zeroes the count");

    fs::remove_all(root);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
