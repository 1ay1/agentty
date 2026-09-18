// AtomicSnapshot on a standard library WITHOUT std::atomic<shared_ptr<T>>.
//
// libc++ has not shipped P0718R2 (as of 2026), so on Termux/Android the
// declaration falls through to the primary atomic template and fails with
// "_Atomic cannot be applied to type ... which is not trivially copyable".
// snapshot.hpp keeps the same public API on a mutex there.
//
// ── Why this is a SEPARATE binary that links nothing ────────────────────────
//
// AGENTTY_FORCE_SNAPSHOT_MUTEX changes AtomicSnapshot's LAYOUT: one variant
// holds a std::atomic<shared_ptr>, the other a mutex plus a plain shared_ptr.
// Defining it in a TU that also links agentty's objects — which were compiled
// with the atomic layout — is a one-definition-rule violation: the same class
// with two different definitions in one program. The linker doesn't complain;
// it just picks one, and the result hangs at static-init time.
//
// That is exactly what happened when this was first written as a folded test,
// and it cost a release to find. So: no agentty objects, no shared object set,
// nothing but this header. If you are tempted to add a link dependency here,
// the ODR problem comes back.
//
// The define comes from the BUILD (AgenttyTests.cmake), not from a #define in
// this file, for a second reason: the fold compiles each test with
// -Dmain=<name>_main, and a macro written here would land on the wrong side of
// that rename.

#include "agentty/util/snapshot.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

} // namespace

int main() {
    static_assert(AGENTTY_HAS_ATOMIC_SHARED_PTR == 0,
                  "this test must compile the mutex fallback, not the atomic "
                  "path — check AGENTTY_FORCE_SNAPSHOT_MUTEX reached the "
                  "compile (it is set in AgenttyTests.cmake, not in this file)");

    using Snap = agentty::util::AtomicSnapshot<std::vector<std::string>>;

    // Empty is distinguishable from published-but-empty. The pickers render
    // "indexing…" vs "workspace is empty" off exactly this.
    Snap s;
    check(!s.has_value(), "a fresh snapshot has no value");
    s.store(std::vector<std::string>{});
    check(s.has_value(), "publishing an empty vector still counts as published");
    check(s.load()->empty(), "…and the payload really is empty");

    s.store(std::vector<std::string>{"a", "b", "c"});
    check(s.load()->size() == 3, "store then load round-trips");

    // The property the whole type exists for: a reader holding a handle keeps
    // its own generation alive across a republish. If this breaks, callers get
    // a dangling view — the bug an exposed shared_ptr invites.
    auto held = s.load();
    s.store(std::vector<std::string>{"x"});
    check(held->size() == 3, "an old handle is unaffected by a new store");
    check(s.load()->size() == 1, "a fresh load sees the new generation");

    // Readers and a writer at once. With the mutex fallback the danger is a
    // torn refcount, which shows up as a crash or a leak rather than a wrong
    // value — so this runs long enough to hit it.
    {
        Snap hot;
        hot.store(std::vector<std::string>{"seed"});
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0};
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    auto h = hot.load();
                    if (h) reads.fetch_add(static_cast<long>(h->size()),
                                           std::memory_order_relaxed);
                }
            });
        }
        for (int i = 0; i < 5000; ++i)
            hot.store(std::vector<std::string>(static_cast<std::size_t>(i % 8) + 1, "v"));
        stop.store(true);
        for (auto& t : readers) t.join();
        check(reads.load() > 0, "concurrent readers saw published generations");
        check(hot.has_value(), "the slot survives a write storm");
    }

    s.reset();
    check(!s.has_value(), "reset clears the published flag");
    check(held->size() == 3, "reset does not disturb a handle taken earlier");

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
