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

#include <string>

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

// Many legacy tests assert through a hand-rolled `void check(bool, const char*)`
// helper + a g_fails counter instead of a macro. Provide that function here so
// those tests migrate by DELETING their local check()/g_fails and wrapping the
// old main() body in a TEST_CASE — the check(...) calls in the body then route
// into doctest unchanged. Overloaded for the (bool) and (bool, msg) forms and
// the occasional (bool, std::string).
namespace agtest {
inline void check(bool ok) { DOCTEST_CHECK(ok); }
inline void check(bool ok, const char* what) { DOCTEST_CHECK_MESSAGE(ok, what); }
inline void check(bool ok, const std::string& what) {
    DOCTEST_CHECK_MESSAGE(ok, what);
}
} // namespace agtest
using agtest::check;

// Scoped process-global sandbox: saves HOME and the current working directory
// on construction and restores them on destruction. Tests that point the
// process at a temp HOME/cwd (setenv/chdir) MUST wrap that in one of these at
// the top of their TEST_CASE, so the mutation doesn't leak into sibling cases
// once every test shares one binary. RAII — restores even if a CHECK throws.
#include <cstdlib>
#include <filesystem>
namespace agtest {
class ScopedEnvSandbox {
public:
    ScopedEnvSandbox() {
        if (const char* h = std::getenv("HOME")) old_home_ = h, had_home_ = true;
        std::error_code ec;
        old_cwd_ = std::filesystem::current_path(ec);
    }
    ~ScopedEnvSandbox() {
        if (had_home_) ::setenv("HOME", old_home_.c_str(), 1);
        else           ::unsetenv("HOME");
        std::error_code ec;
        std::filesystem::current_path(old_cwd_, ec);
    }
    ScopedEnvSandbox(const ScopedEnvSandbox&) = delete;
    ScopedEnvSandbox& operator=(const ScopedEnvSandbox&) = delete;
private:
    std::string old_home_;
    bool had_home_ = false;
    std::filesystem::path old_cwd_;
};
} // namespace agtest
using agtest::ScopedEnvSandbox;

#endif // AGENTTY_TESTS_AGTEST_HPP
