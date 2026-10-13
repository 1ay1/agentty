// Is the sandbox config safe to read from many threads while it is installed?
//
// This file's original subject no longer exists, and the reason is worth
// keeping. The config used to be republishable: SandboxSave wrote a new
// policy from the reducer thread while tools read it on task_isolated
// workers, and `g_cfg = cfg` on a struct holding three vectors is a reader
// walking a freed buffer. That race was real and TSan flagged it.
//
// It is now impossible by construction rather than by careful publishing:
// set_config() is SEALED after the first call, so the policy is written once
// at startup and never again. A boundary must not move under a running
// process -- see set_config's comment for why that is a security property
// and not just a threading convenience.
//
// So what is left to check is narrower but still worth pinning:
//
//   1. the seal HOLDS under concurrent pressure (many threads racing to
//      install; exactly one wins, and the winner's policy is coherent)
//   2. readers taking a snapshot always see one whole policy, never a
//      half-written one
//   3. a held snapshot stays valid and unchanged for its whole lifetime
//
// Run under TSan (this is in the sanitizer lane); the checks below also catch
// a torn read without it.

#include <doctest/doctest.h>

#include "agentty/tool/util/sandbox.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace sb  = agentty::tools::util::sandbox;
namespace cfgn = agentty::sandbox_cfg;

TEST_CASE("sandbox config: the seal holds under a concurrent install storm") {
    // Many threads try to install different policies at once. The seal is a
    // single exchange, so exactly one must win -- and crucially the winner's
    // policy must be WHOLE. A half-applied winner would be the same class of
    // bug the old republishing had, just at startup instead of mid-session.
    sb::reset_config_for_test();

    constexpr int kThreads = 8;
    std::vector<cfgn::Config> candidates;
    for (int i = 0; i < kThreads; ++i) {
        cfgn::Config c;
        c.configured = true;
        // Every field keyed off i, so a mixed result is detectable: a
        // coherent snapshot has all fields from ONE candidate.
        c.memory_mb  = static_cast<std::uint64_t>(1000 + i);
        c.max_procs  = static_cast<std::uint32_t>(i);
        c.read_paths = {"/cand/" + std::to_string(i)};
        candidates.push_back(std::move(c));
    }

    std::atomic<int> ready{0};
    std::vector<std::thread> installers;
    for (int i = 0; i < kThreads; ++i) {
        installers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_relaxed) < kThreads) {}
            sb::set_config(candidates[static_cast<std::size_t>(i)]);
        });
    }
    for (auto& t : installers) t.join();

    CHECK(sb::config_sealed());

    // Whoever won, the result is exactly one candidate -- not a blend.
    const auto got = sb::config_snapshot();
    REQUIRE(got != nullptr);
    const int idx = static_cast<int>(got->memory_mb) - 1000;
    REQUIRE(idx >= 0);
    REQUIRE(idx < kThreads);
    const auto& want = candidates[static_cast<std::size_t>(idx)];
    CHECK(got->max_procs  == want.max_procs);
    CHECK(got->read_paths == want.read_paths);
}

TEST_CASE("sandbox config: concurrent readers always see one whole policy") {
    // The spawn path reads a dozen fields off one snapshot. Under the seal
    // nothing rewrites the config while they do, so this is now a check that
    // the read path itself is sound -- and it is the case that would fail
    // loudest if anyone ever made the policy republishable again.
    sb::reset_config_for_test();

    cfgn::Config c;
    c.configured = true;
    c.memory_mb  = 4096;
    c.net_mode   = cfgn::NetMode::None;
    c.read_paths = {"/one", "/two", "/three"};
    sb::set_config(c);

    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0};
    std::atomic<long> reads{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto snap = sb::config_snapshot();
                if (!snap) { torn.fetch_add(1); continue; }

                if (snap->memory_mb != 4096 ||
                    snap->net_mode != cfgn::NetMode::None ||
                    snap->read_paths.size() != 3) {
                    torn.fetch_add(1);
                    continue;
                }
                // Touch the heap members, so a freed buffer is actually
                // dereferenced rather than merely held.
                std::size_t n = 0;
                for (const auto& s : snap->read_paths) n += s.size();
                if (n == 0) torn.fetch_add(1);

                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Meanwhile something keeps TRYING to change the policy, the way a
    // settings reducer would. Every one of these must be refused, so the
    // readers above must never observe a different value.
    std::thread writer([&] {
        cfgn::Config other;
        other.configured = true;
        other.memory_mb  = 1;
        other.net_mode   = cfgn::NetMode::Full;
        other.read_paths = {"/evil"};
        for (int i = 0; i < 4000; ++i) sb::set_config(other);
        stop.store(true, std::memory_order_relaxed);
    });

    writer.join();
    for (auto& t : readers) t.join();

    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
    // And the refusals really were refusals.
    CHECK(sb::config().memory_mb == 4096);
    CHECK(sb::config().net_mode == cfgn::NetMode::None);
}

TEST_CASE("sandbox config: a reader's snapshot stays valid and unchanged") {
    // A command must run inside the boundary it STARTED with. Under the seal
    // that is trivially true for the whole process, but the snapshot contract
    // is what makes it true even across a reset (which only tests do).
    sb::reset_config_for_test();

    cfgn::Config first;
    first.configured = true;
    first.memory_mb  = 777;
    first.read_paths = {"/first/path"};
    sb::set_config(first);

    const auto held = sb::config_snapshot();
    REQUIRE(held != nullptr);

    // Reinstall repeatedly (breaking the seal each time, as only a test can)
    // and drop the old values.
    for (int i = 0; i < 64; ++i) {
        cfgn::Config next;
        next.configured = true;
        next.memory_mb  = static_cast<std::uint64_t>(i);
        next.read_paths = {"/other/" + std::to_string(i)};
        sb::reset_config_for_test();
        sb::set_config(next);
    }

    // The held snapshot is unchanged and its heap members still valid.
    CHECK(held->memory_mb == 777);
    REQUIRE(held->read_paths.size() == 1);
    CHECK(held->read_paths[0] == "/first/path");

    // And the live value did move on, so this is not passing by accident.
    CHECK(sb::config_snapshot()->memory_mb != 777);
}

TEST_CASE("sandbox config: snapshot is never null, even untouched") {
    // Callers dereference without checking, so the default has to be a real
    // config rather than nothing. A default config is the right answer: it is
    // exactly the posture the bwrap path has always built.
    sb::reset_config_for_test();
    const auto snap = sb::config_snapshot();
    REQUIRE(snap != nullptr);
    // Reading it must not trap.
    (void)snap->net_mode;
    (void)snap->read_paths.size();
    // An untouched config is also not "configured", which is what stops an
    // upgrade from applying a policy nobody set.
    CHECK(!snap->configured);
}
