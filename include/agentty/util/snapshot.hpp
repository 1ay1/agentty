#pragma once
// agentty::util::Snapshot<T> — an immutable, shared, never-dangling view of a
// value that a background thread republishes.
//
// ── The design flaw this closes ──────────────────────────────────────────
//
// The `@` and `#` pickers both read a process-wide cache that a background
// walk fills in. Two sibling files implemented the same cache and disagreed
// on the ONE thing that matters:
//
//   files.cpp    std::vector<std::string> list_workspace_files(...)   // by value
//   symbols.cpp  const std::vector<SymbolEntry>& list_workspace_symbols(...)
//
// The second is a use-after-free waiting to happen, and the body shows why:
//
//     { std::lock_guard lk(sym_mu()); if (auto c = sym_cache()) return *c; }
//     //                                  ^ the shared_ptr COPY that keeps
//     //                                    the buffer alive dies right here,
//     //                                    at the closing brace — and the
//     //                                    reference escapes anyway.
//
// It is benign only because that cache happens to be write-once today. Add
// one invalidation (a refresh, a workspace switch, a cap change) and it is a
// live UAF. Meanwhile the by-value sibling is safe but copies the whole
// vector on every call.
//
// Neither call site did anything wrong locally. The flaw is that "hand back a
// value a background thread may replace" had no TYPE, so each site invented
// its own answer and one of them invented a dangling one.
//
// Snapshot<T> is that type. It holds a shared_ptr<const T>, so:
//
//   • the reader's copy KEEPS THE BUFFER ALIVE for as long as it is held —
//     a republish swaps the cache's pointer, it does not free what a reader
//     is looking at;
//   • handing one out is O(1) (a refcount bump), not a deep copy;
//   • `operator*` / `operator->` are only reachable through the owning
//     handle, so there is no way to spell "reference that outlives the
//     lifetime that justified it";
//   • `empty()` collapses the null case, so a cold cache reads as an empty
//     range instead of requiring every caller to null-check.
//
// The result: correctness (no UAF) and speed (no copy) stop being in
// tension, which is what made the two siblings pick opposite sides.
//
// ── THREADING CONTRACT ── read this before sharing one ──────────────────
//
// A Snapshot has the same thread-safety as a std::shared_ptr, and for the
// same reason: the POINTEE is immutable and freely shared, but the HANDLE is
// an ordinary object.
//
//   SAFE    any number of threads reading the SAME Snapshot object, or each
//           holding their own copy. This is the picker's case — a reader
//           copies the handle once and its buffer cannot be freed underneath
//           it however many times the producer republishes.
//
//   A RACE  one thread copying a Snapshot while another ASSIGNS to that same
//           Snapshot object. That is a read and a write of one `ptr_`, and no
//           amount of pointee immutability fixes it. TSan flags it as a
//           heap-use-after-free, which is exactly what it is.
//
// If a slot is written by a producer and read by consumers, it is not a bare
// Snapshot — it is an AtomicSnapshot (below). The distinction is in the type
// so "which one do I need" is answered at the declaration rather than
// discovered under a sanitizer.

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <version>

// std::atomic<std::shared_ptr<T>> is C++20 (P0718R2), and libstdc++ has had
// it since GCC 12 — but libc++ still has not shipped it, so on Termux/Android
// (and any other libc++ target) the declaration falls through to the PRIMARY
// atomic template, which static_asserts on trivially-copyable and fails with
// a wall of instantiation notes.
//
// Gate on the feature-test macro rather than the compiler: the question is
// "does this standard library have the specialisation", and __cpp_lib_atomic_
// shared_ptr is exactly that question. When it is missing we keep the SAME
// public API backed by a mutex — measurably slower under contention, but this
// slot is written by one background job and read a handful of times per
// frame, so the cost is a couple of uncontended lock/unlock pairs.
// AGENTTY_FORCE_SNAPSHOT_MUTEX=1 forces the fallback on a library that has
// the specialisation, so the path libc++ users actually run is exercised in
// CI on the machines we develop on. Without it, the fallback would only ever
// be compiled by the people least able to report a bug in it.
#if defined(AGENTTY_FORCE_SNAPSHOT_MUTEX) && AGENTTY_FORCE_SNAPSHOT_MUTEX
#  define AGENTTY_HAS_ATOMIC_SHARED_PTR 0
#elif defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#  define AGENTTY_HAS_ATOMIC_SHARED_PTR 1
#else
#  define AGENTTY_HAS_ATOMIC_SHARED_PTR 0
#endif

namespace agentty::util {

template <class T>
class Snapshot {
public:
    // A default Snapshot is EMPTY, not null-and-dangerous. Every accessor
    // below is well-defined on it, so "the background walk has not published
    // yet" needs no special-casing at any call site.
    Snapshot() = default;

    explicit Snapshot(std::shared_ptr<const T> p) noexcept : ptr_(std::move(p)) {}

    // Has a value been published yet? Distinct from `empty()`: a published
    // but genuinely empty workspace is `has_value() && empty()`, which is
    // exactly the distinction the picker's "indexing…" vs "workspace empty"
    // hint needs — and which the old bool-returning `files_ready()` could not
    // express without a second function.
    [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(ptr_); }
    explicit operator bool() const noexcept { return has_value(); }

    // Borrowed view. Safe on an unpublished snapshot: yields a reference to a
    // shared empty instance rather than dereferencing null.
    [[nodiscard]] const T& operator*() const noexcept { return get(); }
    [[nodiscard]] const T* operator->() const noexcept { return &get(); }

    [[nodiscard]] const T& get() const noexcept {
        if (ptr_) return *ptr_;
        static const T kEmpty{};
        return kEmpty;
    }

    // Range access, so a Snapshot<std::vector<X>> works directly in a
    // range-for and in the std algorithms the pickers already use.
    [[nodiscard]] auto begin() const noexcept { return get().begin(); }
    [[nodiscard]] auto end()   const noexcept { return get().end(); }
    [[nodiscard]] std::size_t size()  const noexcept { return get().size(); }
    [[nodiscard]] bool        empty() const noexcept { return get().empty(); }

    [[nodiscard]] decltype(auto) operator[](std::size_t i) const noexcept {
        return get()[i];
    }

    // The owning pointer, for AtomicSnapshot's store(). Not part of the
    // reading surface — callers work through the accessors above.
    [[nodiscard]] std::shared_ptr<const T> share() const noexcept { return ptr_; }

private:
    std::shared_ptr<const T> ptr_;
};

// Publish `value` as an immutable snapshot. The `const` in the shared_ptr is
// the load-bearing part: once published, a snapshot cannot be mutated behind
// a reader's back, so readers need no lock at all — only the pointer swap
// inside the cache does.
template <class T>
[[nodiscard]] Snapshot<T> make_snapshot(T value) {
    return Snapshot<T>{std::make_shared<const T>(std::move(value))};
}

// ── AtomicSnapshot<T> ── a Snapshot slot that a producer republishes ──────
//
// THE type for "one thread publishes, others read", which is every cache in
// this codebase that a background job fills: the workspace file list, the
// symbol index, the git-status map.
//
// Writing that as a bare `Snapshot` field plus a mutex is what every one of
// those sites did by hand, and the hand-rolled versions disagreed about
// whether the LOAD needed the lock too (it does — copying a handle reads
// `ptr_`). Getting it wrong is silent: the pointee really is immutable, so
// the code looks obviously fine and races only on the handle.
//
// std::atomic<std::shared_ptr<T>> does the refcount manipulation atomically,
// so load() hands back a handle that is already safely owned. Lock-free on
// platforms that support it, a tiny internal lock where they don't — either
// way the caller cannot get it wrong, because there is no exposed `ptr_` to
// race on.
//
// On a standard library without that specialisation (libc++ as of 2026 —
// Termux, Android) the same guarantee is provided by a mutex around the
// handle. The PUBLIC SURFACE IS IDENTICAL either way, which is the point:
// the portability problem is solved once, here, instead of at each of the
// three cache sites deciding for themselves.
template <class T>
class AtomicSnapshot {
public:
    AtomicSnapshot() = default;

    // Take a private handle. Safe to call concurrently with store(): the
    // returned Snapshot owns its buffer for as long as the caller holds it.
    [[nodiscard]] Snapshot<T> load() const noexcept {
#if AGENTTY_HAS_ATOMIC_SHARED_PTR
        return Snapshot<T>{cell_.load(std::memory_order_acquire)};
#else
        std::lock_guard lk(mu_);
        return Snapshot<T>{cell_};
#endif
    }

    // Republish. Readers already holding a handle keep seeing their own
    // generation; new loads see this one.
    void store(Snapshot<T> s) noexcept {
#if AGENTTY_HAS_ATOMIC_SHARED_PTR
        cell_.store(s.share(), std::memory_order_release);
#else
        std::lock_guard lk(mu_);
        cell_ = s.share();
#endif
    }

    void store(T value) { store(make_snapshot(std::move(value))); }

    // Has anything been published yet? Distinct from an empty payload — the
    // "indexing…" vs "workspace empty" distinction the pickers render.
    [[nodiscard]] bool has_value() const noexcept {
#if AGENTTY_HAS_ATOMIC_SHARED_PTR
        return static_cast<bool>(cell_.load(std::memory_order_acquire));
#else
        std::lock_guard lk(mu_);
        return static_cast<bool>(cell_);
#endif
    }

    void reset() noexcept {
#if AGENTTY_HAS_ATOMIC_SHARED_PTR
        cell_.store(std::shared_ptr<const T>{}, std::memory_order_release);
#else
        std::lock_guard lk(mu_);
        cell_.reset();
#endif
    }

private:
#if AGENTTY_HAS_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const T>> cell_;
#else
    // `mutable` because load()/has_value() are logically const reads that
    // must still take the lock — copying a shared_ptr WRITES its refcount,
    // which is the race the mutex exists to prevent.
    mutable std::mutex              mu_;
    std::shared_ptr<const T>        cell_;
#endif
};

} // namespace agentty::util
