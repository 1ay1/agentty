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

    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
