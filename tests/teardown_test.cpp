// SPDX-License-Identifier: Apache-2.0
//
// teardown_test.cpp — the process-exit shutdown registry.
//
// Why this exists: main() used to discharge "join every background thread"
// with a hand-written list at the bottom of the function. settings_cache
// wrote a stop function, documented it as "called during teardown", and no
// call was ever added — so its worker sat joinable inside a function-local
// static and ~std::thread called std::terminate at exit (verified: the probe
// aborted with exit code 134).
//
// A hand-maintained list cannot catch that; it IS the thing a hand-maintained
// list forgets. Subsystems register themselves at the moment they acquire a
// thread, and main() drains the registry from a scope guard so every early
// return (~20 CLI subcommands) is covered too.

#include "agentty/util/teardown.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <vector>

namespace teardown = agentty::util::teardown;

TEST_CASE("teardown: actions run LIFO and the registry drains") {
    teardown::run();                       // start from a clean registry

    std::vector<int> order;
    teardown::on_shutdown("first",  [&] { order.push_back(1); });
    teardown::on_shutdown("second", [&] { order.push_back(2); });
    teardown::on_shutdown("third",  [&] { order.push_back(3); });

    CHECK(teardown::pending() == 3);
    teardown::run();

    // LIFO, like static destruction: a subsystem registered later may depend
    // on an earlier one, so it tears down first.
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 3);
    CHECK(order[1] == 2);
    CHECK(order[2] == 1);

    // Drained — a second run is a no-op rather than a double-join.
    CHECK(teardown::pending() == 0);
    teardown::run();
    CHECK(order.size() == 3);
}

TEST_CASE("teardown: a throwing action does not stop the others") {
    // A throw during teardown is worse than the failure it reports, so run()
    // swallows and continues to the next action.
    teardown::run();

    std::atomic<int> ran{0};
    teardown::on_shutdown("ok_after",  [&] { ran.fetch_add(1); });
    teardown::on_shutdown("thrower",   []  { throw std::runtime_error("nope"); });
    teardown::on_shutdown("ok_before", [&] { ran.fetch_add(1); });

    teardown::run();
    CHECK(ran.load() == 2);
    CHECK(teardown::pending() == 0);
}

TEST_CASE("teardown: cancel removes a registration") {
    // An owner whose lifetime is shorter than the process must be able to
    // withdraw, or the registry would hold a callback into freed memory —
    // trading the abort this registry prevents for a use-after-free at exit.
    teardown::run();

    std::atomic<int> ran{0};
    const auto token = teardown::on_shutdown("scoped", [&] { ran.fetch_add(1); });
    teardown::on_shutdown("kept", [&] { ran.fetch_add(1); });
    CHECK(teardown::pending() == 2);

    teardown::cancel(token);
    CHECK(teardown::pending() == 1);

    teardown::run();
    CHECK(ran.load() == 1);          // only the kept one fired

    // Idempotent, and safe with a token whose action already ran.
    teardown::cancel(token);
    teardown::cancel(0);
}
