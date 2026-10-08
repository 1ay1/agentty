#pragma once
// agentty::persistence::SharedFile — the one critical section for a file two
// agentty instances both read-modify-write.
//
// THE POLICY, AND WHY IT IS AGENTTY'S AND NOT JAAL'S
//
// jaal owns the primitive: maya::guarded<T> for threads, and
// maya::platform::native_file_lock for processes. Neither knows WHICH of
// agentty's files are shared, what to do when a lock cannot be taken, or
// that both locks are needed at once. Those are policy, they differ per
// file, and they live here.
//
// BOTH LOCKS, ALWAYS. NEITHER IS SUFFICIENT.
//
// This is the part that was wrong three different ways before this type
// existed, so it is worth stating plainly:
//
//   * A std::mutex is invisible to the other process. agentty's thread index
//     had one, and its comment correctly described the lost update it was
//     closing — between THREADS. With two agentty windows open, both
//     instances read the same index, both wrote their own stale copy, and
//     the second one landed silently dropping the first's entry. The file
//     never corrupted (the write renames), so the damage was a thread
//     missing from the picker.
//
//   * A file lock is invisible to the other THREAD. POSIX record locks are
//     owned by the process: two of agentty's own threads both acquire and
//     neither waits. jaal's conformance suite pins this (check 13,
//     "same-process acquire does NOT block"), so it is a documented
//     property rather than a surprise. Replacing the mutex with a file lock
//     would not have fixed the race, it would have widened it.
//
// So a SharedFile takes the process-wide mutex AND the cross-process lock,
// in that order, for its whole lifetime. Lock ordering is trivially safe
// because there is exactly one of these held at a time — never nest them.
//
// DEGRADE, DO NOT FAIL
//
// Acquisition can fail for reasons the user cannot act on: a read-only HOME,
// a filesystem without lock support, an exhausted descriptor table. When it
// does we log ONCE per file and proceed holding the mutex alone — i.e. we
// fall back to exactly the old behaviour. A lost update in a picker cache is
// a bad day; refusing to save the user's conversation because a lock file
// could not be created is a worse one. `cross_process()` reports which
// guarantee the caller actually got, for a diagnostic that does not lie.

#include <filesystem>
#include <mutex>
#include <optional>

#include <maya/runtime.hpp>

namespace agentty::persistence {

class SharedFile {
  public:
    /// Enter the critical section for `target`, blocking until it is ours.
    /// `local` is the process-wide mutex for that same file; the caller owns
    /// it (one static per file) because a mutex keyed off the path would
    /// need its own lock to look up.
    SharedFile(std::mutex& local, const std::filesystem::path& target);

    SharedFile(const SharedFile&)            = delete;
    SharedFile& operator=(const SharedFile&) = delete;

    /// Did we get the cross-process guarantee, or only the in-process one?
    /// False means another instance can still interleave with us.
    [[nodiscard]] bool cross_process() const noexcept { return cross_.has_value(); }

  private:
    std::unique_lock<std::mutex>                     local_;
    std::optional<maya::platform::native_file_lock>  cross_;
};

}  // namespace agentty::persistence
