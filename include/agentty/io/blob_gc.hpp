#pragma once
// agentty::blobs::gc — reclaim payloads no thread references any more.
//
// WHY THIS IS NOT REFCOUNTING
//
// Blobs are content-addressed: the same screenshot pasted into two
// threads, or an identical tool output, is ONE file referenced from both.
// Measured on a real store: 9 of 188 live blobs have more than one
// referrer. So "delete this thread's blobs when the thread is deleted" is
// wrong — it silently blanks images in threads that are still open, and
// the damage is invisible until someone scrolls back to that turn.
//
// A refcount would fix that in principle and be worse in practice: it
// would have to be updated transactionally on every save, rewrite, fork,
// compaction and delete, and a single missed decrement leaks forever
// while a single spurious one destroys data. The store's whole appeal is
// that a blob is an immutable file named by its content and nothing else
// needs to be consistent with anything.
//
// So: mark and sweep. Walk every thread, collect every reference, delete
// what is left over. O(corpus) and therefore not something to run on a
// hot path — but it is the only design where being WRONG about a
// reference costs disk rather than history.
//
// WHY IT IS NEEDED AT ALL
//
// Every path that shortens a thread orphans blobs: delete_thread,
// compaction, message edit, fork, and the log's rewrite(). Measured on a
// real store after a day's use, 283 of 471 blobs (10.5 MB of 15.5 MB)
// were already unreferenced. That is 60% of the store, and it only grows.
//
// THE SAFETY RULE
//
// The sweep deletes a blob only when NO thread file mentions it. If the
// walk cannot read a thread, it does not know what that thread
// references, so it refuses to delete anything at all. An unreadable
// thread file is exactly when a user most needs their payloads intact.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace agentty::blobs {

struct GcStats {
    std::size_t   scanned_threads = 0;
    std::size_t   unreadable      = 0;  // >0 ⇒ nothing was deleted
    std::size_t   total_blobs     = 0;
    std::size_t   referenced      = 0;
    std::size_t   deleted         = 0;
    std::size_t   dangling        = 0;  // referenced but missing — a real bug
    std::uintmax_t bytes_freed    = 0;
    bool          ran             = false;  // false ⇒ aborted, see `unreadable`

    [[nodiscard]] std::size_t orphans() const noexcept {
        return total_blobs - referenced;
    }
};

// Mark and sweep the blob store. `dry_run` reports what WOULD be deleted
// without touching anything, which is how this should be exercised before
// anyone trusts it with a real store.
//
// Not called automatically from any hot path. Reclaiming disk is never
// urgent enough to justify an O(corpus) walk during a thread switch.
[[nodiscard]] GcStats collect(bool dry_run = false);

// `min_age`: only reclaim unreferenced blobs whose mtime is at least this
// old. A save writes its blobs BEFORE the thread file that references
// them (and another agentty process may be mid-save), so a fresh
// unreferenced blob may be about to become referenced. 0 = no grace.
[[nodiscard]] GcStats collect_in(const std::filesystem::path& threads_dir,
                                 bool dry_run = false,
                                 std::chrono::seconds min_age =
                                     std::chrono::seconds{0});

// Background-safe housekeeping: runs collect with a 24 h grace window, at
// most once per day (stamp file in the blob dir). Returns nullopt when
// skipped because it ran recently.
[[nodiscard]] std::optional<GcStats> collect_if_due();

} // namespace agentty::blobs
