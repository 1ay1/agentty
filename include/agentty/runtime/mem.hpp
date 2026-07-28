#pragma once
// Memory-pressure release helper.
//
// Long sessions accumulate hundreds of MiB of small allocations: every
// tool result string, every Element-tree node in the view cache, every
// scratch JSON parsed off the wire.  When the user takes an action that
// frees a *large* contiguous chunk of that — compacting the conversation
// (drops every prior tool output), switching to / starting a new thread
// (releases the previous thread's view cache) — the bytes go back to the
// allocator's free list but not necessarily back to the kernel.  RSS stays
// high even though the program is logically much smaller.
//
// `release_to_kernel()` is the explicit nudge.  It is intentionally
// scoped: we don't call it on every Tick (would thrash the heap), only
// at the few coarse-grained boundaries where the user has just done
// something that frees megabytes of conversation state.
//
//   • jetalloc     → jet::trim(): releases empty slab pages and the large-
//                    allocation caches back to the OS. Safe to call at any
//                    time; a no-op when there is nothing to reclaim. This is
//                    the path agentty takes by default (the vendored
//                    allocator, linked into the exe). We call it through the
//                    modern C++ facade jetalloc.hpp (jet::trim / jet::stats)
//                    rather than the raw C ABI.
//   • glibc        → malloc_trim(0): walks the main arena, returns fully-free
//                    pages to the kernel via madvise. Fallback when built
//                    without jetalloc (AGENTTY_USE_JETALLOC=OFF).
//   • other        → no-op on musl / macOS / Windows system allocators
//                    (they manage their own return-to-OS policy).
//
// The function is declared `inline` and lives in this header so the
// platform branch is resolved at compile-time per TU; no link-time
// dispatch overhead.

#if defined(AGENTTY_USE_JETALLOC)
#  include <jetalloc.hpp>
#elif defined(__GLIBC__)
#  include <malloc.h>
#endif

#include <cstddef>
#include <optional>

namespace agentty {

inline void release_to_kernel() noexcept {
#if defined(AGENTTY_USE_JETALLOC)
    jet::trim();
#elif defined(__GLIBC__)
    ::malloc_trim(0);
#else
    // No-op on musl / macOS / Windows.
#endif
}

// Typed live-memory snapshot, when the vendored allocator is in play.
// Returns std::nullopt on builds without jetalloc (system allocator exposes
// no equivalent portable counter set). Handy for a debug/status readout or a
// memory-pressure heuristic — no raw jet_stats struct at the call site.
//
//   if (auto m = agentty::memory_stats())
//       log("live={} pages={}", m->bytes_live, m->pages_active);
#if defined(AGENTTY_USE_JETALLOC)
inline std::optional<jet::stats_t> memory_stats() noexcept {
    return jet::stats();
}
#else
struct memory_stats_unavailable_t {};
inline std::optional<memory_stats_unavailable_t> memory_stats() noexcept {
    return std::nullopt;
}
#endif

} // namespace agentty
