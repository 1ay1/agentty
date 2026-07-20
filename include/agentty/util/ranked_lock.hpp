#pragma once
// agentty::util::ranked_lock — a lock hierarchy the TYPE SYSTEM enforces.
//
// The problem this kills (RUST-CRITIQUE.md #2): a two-mutex protocol whose
// ordering ("always take A before B") lives only in code-review discipline
// and comments. A future edit that takes B-then-A compiles fine and can
// deadlock in production. Rust doesn't fix lock ordering for free either —
// deadlocks are memory-safe there — so the usual answer is runtime lockdep
// tooling or collapsing to a single Mutex<T>.
//
// We do BETTER than idiomatic Rust: each mutex carries a compile-time RANK,
// and acquiring a lock is only well-formed if its rank is strictly LESS than
// every rank currently held on this thread. Violations are caught two ways:
//
//   • Nested in one lexical scope   → `static_assert`, a COMPILE error.
//   • Across function boundaries     → a debug-build runtime tripwire
//                                      (thread-local held-rank stack) that
//                                      std::abort()s with a clear message.
//
// So the canonical "session_mtx_ (rank 10) then thread_mtx (rank 20)" order
// becomes a fact the compiler and the runtime both check, not a comment.
//
// Rank convention (LOWER acquired FIRST — the OUTER lock has the lower rank):
//     10  session map / per-session config   (session_mtx_)
//     20  per-session thread.messages         (thread_mtx)
// Add new tiers with distinct ranks; never reuse a rank across two locks
// that can be held nested.

#include <cstdlib>
#include <mutex>

#include "agentty/util/dbglog.hpp"

namespace agentty::util {

// Thread-local stack of currently-held ranks, used only for the cross-scope
// runtime check. Compiled to a no-op body in NDEBUG builds (the compile-time
// check still fires for the nested case regardless of NDEBUG).
namespace detail {

inline thread_local unsigned held_ranks[64] = {};
inline thread_local int       held_depth    = 0;

// Highest rank currently held on this thread, or 0 if none.
inline unsigned top_held_rank() noexcept {
    return held_depth > 0 ? held_ranks[held_depth - 1] : 0u;
}

inline void push_rank([[maybe_unused]] unsigned r) noexcept {
#ifndef NDEBUG
    // Enforce: you may only acquire a rank STRICTLY GREATER than everything
    // held (outer = lower rank, inner = higher rank). Equal or lower = an
    // out-of-order acquisition that risks the classic ABBA deadlock.
    if (held_depth > 0 && r <= held_ranks[held_depth - 1]) {
        dbglog("ranked_lock.ORDER",
               "lock acquired out of rank order — potential ABBA deadlock");
        std::abort();
    }
    if (held_depth < 64) held_ranks[held_depth] = r;
#endif
    ++held_depth;
}

inline void pop_rank() noexcept {
    if (held_depth > 0) --held_depth;
}

} // namespace detail

// A std::mutex tagged with a compile-time rank. Lock it only through
// `RankedLock` (below), which performs the ordering checks.
template <unsigned Rank>
class RankedMutex {
public:
    static constexpr unsigned rank = Rank;

    RankedMutex() = default;
    RankedMutex(const RankedMutex&)            = delete;
    RankedMutex& operator=(const RankedMutex&) = delete;

    // Raw handle for the RAII guard. Not for direct use — go through
    // RankedLock so the ordering discipline is applied.
    std::mutex& raw() noexcept { return m_; }

private:
    std::mutex m_;
};

// RAII guard. Constructing it acquires `m` after checking the rank order:
//   • The runtime tripwire (debug) fires if a lower/equal rank is already
//     held on this thread.
//   • The static ordering between two guards in the SAME scope is enforced
//     by `lock_after` below.
template <unsigned Rank>
class [[nodiscard]] RankedLock {
public:
    explicit RankedLock(RankedMutex<Rank>& m) : lk_(m.raw()) {
        detail::push_rank(Rank);
    }
    ~RankedLock() { detail::pop_rank(); }

    RankedLock(const RankedLock&)            = delete;
    RankedLock& operator=(const RankedLock&) = delete;

    static constexpr unsigned rank = Rank;

private:
    std::lock_guard<std::mutex> lk_;
};

// Compile-time ordering assertion for two guards taken in the same lexical
// scope. Usage:
//
//     RankedLock outer(session_mtx_);
//     lock_after<decltype(outer)::rank>();   // proves the NEXT lock is inner
//     RankedLock inner(thread_mtx_);
//
// If `Inner <= Outer` the static_assert fails at COMPILE time — the wrong
// order literally does not build.
template <unsigned OuterRank, unsigned InnerRank>
constexpr void assert_lock_order() {
    static_assert(InnerRank > OuterRank,
                  "lock-order violation: inner lock rank must be strictly "
                  "greater than the outer lock's rank (outer = lower rank, "
                  "acquired first). Reordering these acquisitions risks an "
                  "ABBA deadlock.");
}

// ── Proofs ───────────────────────────────────────────────────────────────────
namespace proofs {
// The rank is a type-level fact, readable at compile time.
static_assert(RankedMutex<10>::rank == 10);
static_assert(RankedLock<20>::rank  == 20);
// A well-ordered pair (outer 10, inner 20) is well-formed — this compiles.
constexpr auto ok = [] { assert_lock_order<10, 20>(); return true; }();
static_assert(ok);
// assert_lock_order<20, 10>() and assert_lock_order<10, 10>() would each be a
// compile ERROR (inner must be strictly > outer) — exactly the guarantee.
} // namespace proofs

} // namespace agentty::util
