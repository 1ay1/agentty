// complexity_test — the turn-complexity classifier (smart::classify_complexity).
// Pure heuristic, no I/O. Locks the conservative calibration: Standard is the
// fallback, only strong signals move a turn to Complex or Trivial.

#include "agentty/domain/complexity.hpp"

#include <cstdio>

using namespace agentty::smart;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main() {
    // Trivial: short acknowledgements / one-word imperatives, no question.
    CHECK(classify_complexity("yes") == Complexity::Trivial, "yes → trivial");
    CHECK(classify_complexity("go ahead") == Complexity::Trivial, "go ahead → trivial");
    CHECK(classify_complexity("commit it") == Complexity::Trivial, "commit it → trivial");
    CHECK(classify_complexity("  thanks  ") == Complexity::Trivial, "trimmed thanks → trivial");
    CHECK(classify_complexity("") == Complexity::Trivial, "empty → trivial");

    // Complex: design/debug/why vocabulary, long turns, or many enumerated asks.
    CHECK(classify_complexity("redesign the auth module") == Complexity::Complex,
          "redesign → complex");
    CHECK(classify_complexity("why does the retry loop deadlock?") == Complexity::Complex,
          "why+deadlock → complex");
    CHECK(classify_complexity("do a deep research on this and implement all state of the art way")
          == Complexity::Complex, "deep research + implement all → complex");
    CHECK(classify_complexity(
            "1. add a field\n2. persist it\n3. render it\n4. test it") == Complexity::Complex,
          "4 enumerated asks → complex");

    // Simple: a short single-clause request (no design vocab, one line).
    CHECK(classify_complexity("fix the typo in README") == Complexity::Simple,
          "short fix → simple");
    CHECK(classify_complexity("rename foo to bar") == Complexity::Simple,
          "short rename → simple");

    // Standard: the conservative default for medium, ambiguous turns.
    CHECK(classify_complexity(
            "update the parser to also accept trailing commas in list literals")
          == Complexity::Standard, "medium request → standard");

    // Conservative bias: an ambiguous 'why' still escalates (never under-thinks).
    CHECK(classify_complexity("explain why this test is flaky") == Complexity::Complex,
          "why → escalates (conservative upward)");

    // ── Context inheritance (classify_with_context): a short follow-up to a
    //    Complex turn keeps some weight instead of collapsing to Simple.
    CHECK(classify_with_context("now do the same for the other module",
                                Complexity::Complex) == Complexity::Standard,
          "short follow-up after Complex → lifted to Standard");
    // A fresh Trivial ack is always taken at face value regardless of history.
    CHECK(classify_with_context("thanks", Complexity::Complex) == Complexity::Trivial,
          "ack after Complex → still Trivial (an ack is an ack)");
    // A fresh Complex signal in the text wins outright.
    CHECK(classify_with_context("redesign the whole thing", Complexity::Simple)
          == Complexity::Complex, "fresh Complex text wins over Simple history");
    // Never DROPS below the text's own tier when history is weaker.
    CHECK(classify_with_context("fix the typo", Complexity::Trivial) == Complexity::Simple,
          "weaker history never lowers the text's own tier");
    // Standard history + Simple text stays Simple (only Complex lifts).
    CHECK(classify_with_context("rename foo to bar", Complexity::Standard)
          == Complexity::Simple, "Standard history does not lift a Simple follow-up");

    // ── Script-agnostic size floor: a long non-space-delimited (e.g. CJK)
    //    turn must not perpetually under-rate to Simple just because
    //    word_count sees ~1 whitespace-token. A ~300-byte no-space string
    //    escalates via the byte-length proxy.
    {
        std::string cjk;
        for (int i = 0; i < 100; ++i) cjk += "\xE6\x8E\xA2";   // 3-byte glyph ×100 = 300B
        CHECK(classify_complexity(cjk) == Complexity::Complex,
              "long no-space (CJK-like) turn escalates via byte-length floor");
    }
    // Empty / whitespace-only input is Trivial, never a crash.
    CHECK(classify_complexity("") == Complexity::Trivial, "empty → trivial");
    CHECK(classify_complexity("   \n\t ") == Complexity::Trivial, "whitespace → trivial");

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
