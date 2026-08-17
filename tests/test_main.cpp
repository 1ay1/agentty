// agentty_tests — the single test binary.
//
// Every unit test in tests/ is compiled as a doctest TEST_CASE and linked into
// THIS one executable, which links the shared agentty object set exactly once.
// The previous model built ~70 separate executables, each statically re-linking
// the whole object set — that link fan-out was the dominant CI cost. doctest
// auto-registers every TEST_CASE, so this file only supplies main().
//
// ctest still runs and filters individual cases: doctest_discover_tests()
// registers each TEST_CASE as its own ctest entry (`ctest -j` parallelism and
// per-case failure reporting are preserved), and `agentty_tests --test-case=X`
// runs one in isolation.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <maya/core/anim_clock.hpp>

int main(int argc, char** argv) {
    // Pin the animation clock for the whole binary. Several render/seam tests
    // (midrun_*, turn_settle, reveal) drive frames synchronously and assert on
    // committed-scrollback stability; they require maya::anim_now_ms() frozen
    // so render is a pure function of the model instead of racing wall-clock.
    // Harmless for tests that don't read it. Formerly each such test froze it
    // in its own main(); with one shared binary we do it once here.
    maya::testing::freeze_anim_clock();
    return doctest::Context(argc, argv).run();
}
