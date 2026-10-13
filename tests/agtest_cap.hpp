// agtest::cap() — the suite's single lock-order capability root, split out of
// agtest.hpp so STANDALONE tests (own int main / check(), folded into
// agentty_standalone_tests) can use it without pulling in doctest and the
// CHECK/check shims. Doctest tests keep including agtest.hpp, which includes
// this. See agtest.hpp for the rules of use.
#ifndef AGENTTY_TESTS_AGTEST_CAP_HPP
#define AGENTTY_TESTS_AGTEST_CAP_HPP

#include <maya/runtime.hpp>

namespace agtest {
inline const maya::unlocked& cap() { return maya::test_root(); }
}  // namespace agtest

#endif
