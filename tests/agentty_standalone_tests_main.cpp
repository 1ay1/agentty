// agentty_standalone_tests — one binary, argv-dispatched, for the full-stack
// standalone tests that CANNOT be doctest cases in a shared process (they
// fork/exec, drive a PTY, run fuzz seed loops, or spawn subprocesses). Each is
// still run in its OWN process per ctest entry:  agentty_standalone_tests <name>
// [args…] → the test's original main.  Folds ~16 heavy 100 MB+ links into one.
//
// Each test's `int main(...)` is renamed to `<name>_main` at compile time via a
// per-source `-Dmain=<name>_main` (see cmake/AgenttyTests.cmake). A test's
// ORIGINAL signature is preserved by the rename, so the .def below records each
// test's arity: NOARGS (int main()) vs ARGS (int main(int,char**)).  For an ARGS
// test we pass argv[1..] through so it parses its own flags (anthropic_md_stream:
// `det <fixture> --cps …`).
//
// The 3 sanitizer-narrow tests (concurrency_primitives / cred_crypt / keystore)
// are NOT here — they link a deliberately minimal source subset to stay
// asan-clean, which folding into the full object set would defeat.
#include <cstdio>
#include <string_view>

// Declarations — arity per the original main().
#define FOLD_NOARGS(name) extern int name##_main();
#define FOLD_ARGS(name)   extern int name##_main(int, char**);
#include "agentty_standalone_tests.def"
#undef FOLD_NOARGS
#undef FOLD_ARGS

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <test-name> [args…]\n\nAvailable tests:\n", argv[0]);
#define FOLD_NOARGS(name) std::fprintf(stderr, "  %s\n", #name);
#define FOLD_ARGS(name)   std::fprintf(stderr, "  %s\n", #name);
#include "agentty_standalone_tests.def"
#undef FOLD_NOARGS
#undef FOLD_ARGS
        return 2;
    }
    const std::string_view which{argv[1]};
    // Build the sub-argv: keep argv[0] as the REAL executable path (a test may
    // re-exec itself — external_acp_backend_test spawns argv[0] as a fake ACP
    // agent), drop the dispatch name at argv[1], keep the rest as the test's
    // own flags. So `agentty_standalone_tests anthropic_md_stream det X --cps 45`
    // reaches the test as argv = {<exe-path>, "det", "X", "--cps", "45"}.
    int    sub_argc = argc - 1;
    char** sub_argv = argv + 1;
    sub_argv[0] = argv[0];   // real exe path, not the test name

#define FOLD_NOARGS(name) if (which == #name) return name##_main();
#define FOLD_ARGS(name)   if (which == #name) return name##_main(sub_argc, sub_argv);
#include "agentty_standalone_tests.def"
#undef FOLD_NOARGS
#undef FOLD_ARGS

    std::fprintf(stderr, "%s: unknown test '%.*s'\n",
                 argv[0], static_cast<int>(which.size()), which.data());
    (void)sub_argc; (void)sub_argv;
    return 2;
}
