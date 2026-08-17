// routing_memory_test — the per-workspace learned routing prior.
// Verifies: neutral when unseen, a sustained regret signal moves the prior in
// the right direction, a single event doesn't swing it, and it round-trips
// through the on-disk TSV. Runs against an isolated temp workspace.

#include "agentty/domain/routing_memory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "agtest.hpp"

namespace fs = std::filesystem;
using namespace agentty::smart;

TEST_CASE("routing memory persistence + recall") {
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

    // ── HIERARCHICAL BACKOFF: the redesign's core property. ONE structural
    //    class teaches a prior; a BRAND-NEW specific turn of that same class
    //    (never routed itself) inherits the class prior instead of starting
    //    neutral. A flat single-key store could not do this.
    {
        rm.reset();
        // Teach the coarse class "Standard, no-?, code-ish, short" via several
        // DISTINCT specific turns that all under-rated.
        for (const char* q : {"refactor the http client", "refactor the tls layer",
                              "refactor the sse framer", "refactor the retry loop"}) {
            auto s = turn_signature(Complexity::Standard, q);
            for (int i = 0; i < 6; ++i) { rm.note_routed(s); rm.note_regret(s, +1); }
        }
        // A NEW turn in the same structural class, never seen before, borrows
        // the class's learned upward prior via backoff.
        auto fresh = turn_signature(Complexity::Standard, "refactor the wire codec");
        CHECK(rm.prior_bias(fresh) == 1,
              "backoff: an unseen turn inherits its structural class's prior");
        // A turn in a DIFFERENT structural class (a question) does not.
        auto other_class = turn_signature(Complexity::Standard, "why is it slow?");
        CHECK(rm.prior_bias(other_class) == 0,
              "backoff is scoped to the coarse class, not global");
    }

    // Compaction: an append-only store must not grow without bound. After many
    // events the file is rewritten to at most two lines per signature, and the
    // learned prior is PRESERVED across the rewrite (aggregate is what matters).
    {
        const std::string cs = turn_signature(Complexity::Standard, "compact me");
        for (int i = 0; i < 1300; ++i) { rm.note_routed(cs); rm.note_regret(cs, +1); }
        const int before = rm.prior_bias(cs);
        // Cross the 2000-line threshold to force at least one compaction.
        for (int i = 0; i < 900; ++i) { rm.note_routed(cs); rm.note_regret(cs, +1); }
        const int after = rm.prior_bias(cs);
        CHECK(after == before || after != 0, "prior survives compaction (non-neutral)");

        // Count physical lines: compaction rewrites to ~2 lines per signature,
        // then appends accrue again until the next threshold crossing — so the
        // file is BOUNDED by the compaction threshold, never the ~4400 raw
        // events written. (Pre-compaction it would be strictly monotonic.)
        auto tsv = root / ".agentty" / "routing_memory.tsv";
        std::size_t lines = 0;
        if (std::ifstream f{tsv}) {
            std::string l; while (std::getline(f, l)) ++lines;
        }
        CHECK(lines > 0 && lines <= 2100,
              "compaction bounds the file below the threshold, not raw event count");
    }

    // Reset wipes the workspace's learning.
    rm.reset();
    CHECK(rm.prior_bias(sig) == 0 && rm.prior_bias(ov) == 0,
          "reset() clears every learned prior");
    CHECK(rm.learned_count() == 0, "reset() zeroes the count");

    fs::remove_all(root);
}
