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

#include <cstddef>
#include <memory>
#include <utility>

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

} // namespace agentty::util
