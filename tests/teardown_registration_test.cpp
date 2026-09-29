// SPDX-License-Identifier: Apache-2.0
//
// teardown_registration_test.cpp — every threaded subsystem registers its
// own join.
//
// THE BUG CLASS. main() ends with a hand-written list of joins — one line
// per subsystem someone remembered is threaded. That list is the wrong
// shape: it lives far from the thread it is about, it is only correct
// until the next subsystem is added, and being wrong is silent. It has
// already been wrong once: settings_cache shipped a shutdown() documented
// as "called during teardown" that no call site ever called, so its worker
// sat joinable in a function-local static and ~std::thread terminated the
// process at exit.
//
// util::teardown exists to invert that: a subsystem registers its own join
// where it starts its thread, and main() calls run() without knowing who
// registered. The registry was added but adoption stalled at one
// subsystem, leaving the other two still spelled out in main() — the exact
// arrangement that lost settings_cache.
//
// This test pins the inversion. It starts each threaded subsystem and
// asserts the registry grew, so a NEW subsystem that spawns a thread
// without registering shows up here rather than as a hang at exit six
// months later.

#include "agentty/util/teardown.hpp"
#include "agentty/util/modelsdev.hpp"
#include "agentty/io/blob_gc.hpp"

#include <cstdio>

namespace {
int g_failures = 0;
void expect(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}
}  // namespace

int teardown_registration_test_main() {
    std::printf("=== teardown_registration_test ===\n");
    namespace td = agentty::util::teardown;

    // Baseline: nothing registered by merely linking.
    const std::size_t base = td::pending();

    // Each of these spawns a background thread. Each must register its own
    // join as a side effect of starting -- that is the property under test.
    agentty::modelsdev::start_background_refresh(/*no_net=*/true);
    const std::size_t after_modelsdev = td::pending();
    expect(after_modelsdev > base,
           "modelsdev.start_background_refresh registers its join");

    agentty::blobs::start_background_gc();
    const std::size_t after_blobgc = td::pending();
    expect(after_blobgc > after_modelsdev,
           "blobs.start_background_gc registers its join");

    // Bail out NOW if either registration is missing. Without this the test
    // would run on to td::run(), which would drain a registry that never
    // learned about the unregistered worker -- leaving a joinable
    // std::thread to be destroyed at exit and abort the process with
    // "terminate called without an active exception", swallowing the
    // report. That abort IS the shipped bug, so reproducing it here is
    // right; printing which subsystem caused it first is what makes the
    // test useful rather than merely red.
    if (g_failures) {
        std::printf("\nFAILED (%d): a threaded subsystem did not register its\n"
                    "join with util::teardown. main() cannot join what it is\n"
                    "never told about, so the worker is destroyed joinable at\n"
                    "exit and std::terminate fires. Register in the function\n"
                    "that starts the thread, not in main().\n", g_failures);
        return 1;
    }

    // Starting twice must not register twice: the registry is a list, and a
    // subsystem that re-registers on every call would grow it without bound
    // (and join itself N times at exit).
    agentty::modelsdev::start_background_refresh(true);
    agentty::blobs::start_background_gc();
    expect(td::pending() == after_blobgc,
           "re-starting a subsystem does not re-register");

    // run() must drain the registry and join every worker. If a join
    // deadlocks or a callback touches freed memory, this is where it shows
    // up -- and under TSan, so does any race between the worker and its own
    // join.
    td::run();
    expect(td::pending() == 0, "teardown::run() drains the registry");

    // Idempotent: the TUI path calls run() explicitly AND a scope guard
    // calls it again on the way out.
    td::run();
    expect(td::pending() == 0, "teardown::run() is idempotent");

    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
