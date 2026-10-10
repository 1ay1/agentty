#pragma once
// agentty::persistence::with_shared_file — the one critical section for a file two
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
// So with_shared_file takes the process-wide lane (a guarded) AND the
// cross-process lock, in that order, for the body. Lock ordering is trivially
// safe because there is exactly one of these held at a time, and the lane's
// captureless body cannot reach a second one.
//
// DEGRADE, DO NOT FAIL
//
// Acquisition can fail for reasons the user cannot act on: a read-only HOME,
// a filesystem without lock support, an exhausted descriptor table. When it
// does we log ONCE per file and proceed holding the lane alone — i.e. we
// fall back to exactly the old behaviour. A lost update in a picker cache is
// a bad day; refusing to save the user's conversation because a lock file
// could not be created is a worse one. `Held::cross_process` reports which
// guarantee the caller actually got, for a diagnostic that does not lie.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <maya/runtime.hpp>

#include "agentty/util/lock_levels.hpp"

namespace agentty::persistence {

/// The in-process half, one per shared file: a static the caller owns (one
/// keyed off the path would need its own lock to look up). Only
/// with_shared_file can enter it.
struct FileLane {};
// A lane is the outer lock: its body may take the state it protects.
struct Lane : maya::guarded<FileLane> {
    Lane() : maya::guarded<FileLane>(lock_levels::kFileLane) {}
};

/// What the body of with_shared_file sees.
struct Held {
    /// Did we get the cross-process guarantee, or only the in-process one?
    /// False means another instance can still interleave with us.
    bool cross_process = false;
};

namespace detail {
/// Take the cross-process lock for `target`, or nullopt (logged once per
/// file) when the filesystem cannot lock.
std::optional<maya::platform::native_file_lock> acquire_cross(const std::string& target);
}  // namespace detail

/// Enter the critical section for `target`, both locks, in-process first,
/// run `body(Held, args...)`, and leave. body is captureless and args are
/// moved in, the same rules as guarded<T>::with, since that is what it is.
template <class F, class... Args>
auto with_shared_file(Lane& lane, const std::filesystem::path& target, F body, Args... args) {
    (void)body;
    return lane.with([](FileLane&, std::string t, Args... a) {
        const auto cross = detail::acquire_cross(t);
        return F{}(Held{cross.has_value()}, std::move(a)...);
    }, target.string(), std::move(args)...);
}

}  // namespace agentty::persistence
