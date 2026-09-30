// Is the sandbox config safe to publish while tools are reading it?
//
// This exists because the config was NOT, and the reason is worth stating:
// `g_cfg` was a plain global `sandbox_cfg::Config`, documented as "written
// once before maya starts, read by every bash call after". True until the
// settings pane shipped. SandboxSave publishes a new policy from the REDUCER
// thread, while tools -- every reader of this config -- run on task_isolated
// worker threads. Config holds three std::vectors and a std::string, so
// `g_cfg = cfg` under a concurrent reader is a reader walking a freed
// pointer: torn size/capacity, or a heap-use-after-free on the buffer the
// assignment just replaced.
//
// It is the nastiest class of bug to find by hand -- the window is a few
// instructions wide, it needs a save to land inside a command, and the
// symptom is a crash or a silently wrong boundary somewhere else entirely.
// So it gets a stress test rather than an argument.
//
// Run under TSan (this is in the sanitizer lane) and the race is reported
// directly; run plain and the checks below still catch a torn read, because
// a mixed snapshot fails the coherence assertion.

#include <doctest/doctest.h>

#include "agentty/tool/util/sandbox.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace sb  = agentty::tools::util::sandbox;
namespace cfgn = agentty::sandbox_cfg;

TEST_CASE("sandbox config: publishing under concurrent readers is coherent") {
    // Two policies that differ in EVERY field, so a mixed snapshot is
    // detectable. If a reader could ever see net_mode from one and
    // read_paths from the other, the boundary it builds belongs to neither.
    cfgn::Config a;
    a.configured = true;
    a.net_mode   = cfgn::NetMode::None;
    a.memory_mb  = 1024;
    a.read_paths = {"/aaa", "/aab", "/aac"};

    cfgn::Config b;
    b.configured = true;
    b.net_mode   = cfgn::NetMode::Full;
    b.memory_mb  = 2048;
    b.read_paths = {"/bbb", "/bbc"};

    sb::set_config(a);

    std::atomic<bool> stop{false};
    std::atomic<int>  torn{0};
    std::atomic<long> reads{0};

    // Readers, standing in for tool workers.
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                // ONE snapshot, read repeatedly -- exactly what the spawn
                // path does when it builds a posture out of a dozen fields.
                const auto snap = sb::config_snapshot();
                REQUIRE(snap != nullptr);

                // Every field must belong to the SAME policy. This is the
                // coherence property a plain global cannot give you: with
                // `g_cfg = cfg` a reader can straddle the assignment.
                const bool is_a = snap->memory_mb == 1024;
                const bool is_b = snap->memory_mb == 2048;
                if (!is_a && !is_b) { torn.fetch_add(1); continue; }

                const auto& want_paths = is_a ? a.read_paths : b.read_paths;
                const auto  want_net   = is_a ? a.net_mode   : b.net_mode;
                if (snap->read_paths != want_paths || snap->net_mode != want_net)
                    torn.fetch_add(1);

                // Touch the heap members, so a use-after-free on a replaced
                // buffer is actually dereferenced rather than merely held.
                std::size_t n = 0;
                for (const auto& p : snap->read_paths) n += p.size();
                if (n == 0 && !snap->read_paths.empty()) torn.fetch_add(1);

                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // The writer, standing in for SandboxSave on the reducer thread.
    std::thread writer([&] {
        for (int i = 0; i < 4000; ++i) sb::set_config(i % 2 ? b : a);
        stop.store(true, std::memory_order_relaxed);
    });

    writer.join();
    for (auto& t : readers) t.join();

    CHECK(torn.load() == 0);
    // Make sure the readers actually ran, so a zero above means "no tearing"
    // and not "never looked".
    CHECK(reads.load() > 0);
}

TEST_CASE("sandbox config: a reader's snapshot survives later publishes") {
    // The other half of the guarantee, and the reason snapshots are the right
    // shape rather than just a safe one: a command must run inside the
    // boundary it STARTED with. A save landing mid-command cannot be allowed
    // to change the walls a running command is already inside -- that is a
    // policy nobody chose, assembled from two they did.
    cfgn::Config first;
    first.configured = true;
    first.memory_mb  = 777;
    first.read_paths = {"/first/path"};
    sb::set_config(first);

    const auto held = sb::config_snapshot();
    REQUIRE(held != nullptr);

    // Publish something else several times over, dropping the old value.
    for (int i = 0; i < 64; ++i) {
        cfgn::Config next;
        next.configured = true;
        next.memory_mb  = static_cast<std::uint64_t>(i);
        next.read_paths = {"/other/" + std::to_string(i)};
        sb::set_config(next);
    }

    // The held snapshot is unchanged and its heap members are still valid.
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
    const auto snap = sb::config_snapshot();
    REQUIRE(snap != nullptr);
    // Reading it must not trap.
    (void)snap->net_mode;
    (void)snap->read_paths.size();
}
