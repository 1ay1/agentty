// agtest — thin doctest compatibility layer for agentty's test suite.
//
// The suite historically hand-rolled a `CHECK` macro per file in two shapes:
//   CHECK(cond)            — bare predicate
//   CHECK(cond, msg)       — predicate + human message on failure
// doctest's own CHECK takes only the predicate. This header maps BOTH legacy
// shapes onto doctest so migrating a test is near-zero-diff in the body: drop
// the file's `int main()` and its local `#define CHECK`, include this header,
// and wrap each `void test_x()` in TEST_CASE("x"). The assertions inside are
// left exactly as written.
//
// Include this INSTEAD of <doctest/doctest.h> in migrated tests. The single
// binary's main() still comes from tests/test_main.cpp.
#ifndef AGENTTY_TESTS_AGTEST_HPP
#define AGENTTY_TESTS_AGTEST_HPP

#include <doctest/doctest.h>

// Legacy two-arg CHECK(cond, msg) → doctest CHECK_MESSAGE; one-arg CHECK(cond)
// → doctest CHECK. Both wrap the predicate in an extra paren pair so doctest
// does NOT try to decompose it — the legacy suite freely uses `CHECK(a && b)`
// and `CHECK(x || y)`, which doctest's expression decomposer rejects with
// "Expression Too Complex". Wrapping evaluates the whole thing as one bool
// (we lose per-operand value printing, which the hand-rolled harness never had
// anyway).
#ifdef CHECK
#  undef CHECK
#endif
#define AGTEST_CHECK_2(cond, msg) DOCTEST_CHECK_MESSAGE((cond), msg)
#define AGTEST_CHECK_1(cond)      DOCTEST_CHECK((cond))
#define AGTEST_CHECK_PICK(_1, _2, NAME, ...) NAME
#define CHECK(...) \
    AGTEST_CHECK_PICK(__VA_ARGS__, AGTEST_CHECK_2, AGTEST_CHECK_1)(__VA_ARGS__)

// Same for the REQUIRE spelling some tests use for fatal checks.
#ifdef REQUIRE
#  undef REQUIRE
#endif
#define AGTEST_REQUIRE_2(cond, msg) DOCTEST_REQUIRE_MESSAGE((cond), msg)
#define AGTEST_REQUIRE_1(cond)      DOCTEST_REQUIRE((cond))
#define AGTEST_REQUIRE_PICK(_1, _2, NAME, ...) NAME
#define REQUIRE(...) \
    AGTEST_REQUIRE_PICK(__VA_ARGS__, AGTEST_REQUIRE_2, AGTEST_REQUIRE_1)(__VA_ARGS__)

#endif // AGENTTY_TESTS_AGTEST_HPP
