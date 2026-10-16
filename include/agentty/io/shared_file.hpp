#pragma once
// agentty::persistence — the one way agentty writes a file another agentty
// may also be writing.
//
// jaal owns the primitives: maya::guarded for threads and
// maya::platform::native_file_lock for processes. Which files are shared,
// what to do when a lock can't be had, and how bytes reach the disk are
// agentty's policy, and they live here. Nothing else in agentty calls the
// file lock or renames a temp over a file (tests/lint/disk_writes.py).
//
// BOTH LOCKS, ALWAYS
//
// A guarded is invisible to another process, and a POSIX record lock is
// invisible to another thread of the same process (jaal conformance check
// 13). So a section takes the file's in-process Lane, then the file lock.
//
// LOCK ORDER IS A TYPE
//
// Every Lane has a rank (lock_levels). A section is entered with a
// capability proving the thread holds only lower ranks, so two sections can
// only nest in rank order, and every process takes their file locks in that
// same order. The cross-process order is the in-process order, and the
// compiler checks it.
//
// BOUNDED, AND NEVER FATAL
//
// The file lock waits at most kSharedFileWait (jaal acquire_within, a kernel
// wait that is legal inside a lock body). Past that, or on a filesystem that
// can't lock, the body runs holding the lane alone and Held::cross_process
// is false, logged once per file. A lost update is a bad day; an agentty
// frozen behind a peer stopped with ^Z is worse.
//
// FAST BY DEFAULT
//
// The uncontended cost is one open + one fcntl on the sidecar, and the lane
// is a mutex. write_atomic fsyncs only what must survive a power cut
// (Durability), and creates its temp with the final mode, so no chmod
// window and no extra syscall.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <maya/runtime.hpp>

#include "agentty/util/io.hpp"
#include "agentty/util/lock_levels.hpp"

namespace agentty::persistence {

/// How long a section waits for another process before going on without it.
/// A read-modify-write of one of these files takes well under a millisecond,
/// so a holder past this is stopped, not busy.
inline constexpr std::chrono::milliseconds kSharedFileWait{5000};

/// The in-process half of one shared file: a static the owner keeps next to
/// the file's path. Rank R places it in lock_levels; files whose sections
/// nest need distinct ranks, outer first.
struct FileLane {};
template <class R = lock_levels::file_lane>
using LaneOf = maya::guarded<FileLane, R>;
using Lane   = LaneOf<lock_levels::file_lane>;

/// What the body of a section sees.
struct Held {
    /// Did we get the cross-process guarantee, or only the in-process one?
    /// False means another instance can still interleave with us.
    bool cross_process = false;
};

namespace detail {
/// The file lock for `target`, waited for at most kSharedFileWait. nullopt
/// (logged once per file) when it can't be had.
std::optional<maya::platform::native_file_lock> acquire_cross(const std::string& target);
}  // namespace detail

/// Enter the section for `target`: its lane, then its file lock. Runs
/// `body(const held<R>&, Held, args...)` and leaves. Same rules as
/// guarded<T>::with: body is captureless, args are moved in, and the result
/// must be Sendable. The held<R> lets the body take state ranked above R,
/// including a higher-ranked section.
template <class R, class F, class... Args>
auto with_shared_file(const std::type_identity_t<maya::below<R>>& cap, LaneOf<R>& lane,
                      const std::filesystem::path& target, F body, Args... args) {
    (void)body;
    return lane.with(cap, [](const maya::held<R>& h, FileLane&, std::string t, Args... a) {
        const auto cross = detail::acquire_cross(t);
        return F{}(h, Held{cross.has_value()}, std::move(a)...);
    }, target.string(), std::move(args)...);
}

/// The file locks for several files, for a lane body that rewrites more
/// than one (memory's user + project scope). Taken in sorted path order,
/// the same order in every process, so two can't each hold one and wait
/// for the other. Bounded and best-effort like a section: cross_process()
/// says whether all of them were had. Only inside the owning lane's body.
class FileLocks {
public:
    explicit FileLocks(std::vector<std::filesystem::path> files);
    [[nodiscard]] bool cross_process() const noexcept { return all_; }
    FileLocks(const FileLocks&)            = delete;
    FileLocks& operator=(const FileLocks&) = delete;
private:
    std::vector<maya::platform::native_file_lock> held_;
    bool all_ = true;
};

// ── Writing bytes ────────────────────────────────────────────────────

/// What a write must survive.
enum class Durability {
    /// A power cut may lose the write but never tears the file: temp,
    /// rename. For caches and anything rebuilt from elsewhere.
    Atomic,
    /// Survives a power cut: fsync the data before the rename and the
    /// directory after. For anything the user would lose.
    Durable,
};

struct WriteOptions {
    Durability durability = Durability::Durable;
    /// POSIX mode the file is created with (umask still applies to the
    /// default). 0600 for secrets: the temp is born private, so there is no
    /// window where it's readable. Ignored on Windows.
    unsigned mode = 0644;
    /// Keep the target's existing mode if it has one (user files).
    bool keep_mode = false;
};

/// Atomically replace `target` with `bytes`: a temp in the same directory
/// with a name unique to this process and call, then rename. Readers see
/// the old file or the new one, never a mix. Creates the parent directory.
/// Returns false (temp removed) on any failure.
[[nodiscard]] bool write_atomic(Io, const std::filesystem::path& target,
                                std::string_view bytes, WriteOptions = {});

/// Append `bytes` in ONE write(2) with O_APPEND, so concurrent appenders
/// (threads or processes) never interleave inside a record. `bytes` should
/// be whole records ending in '\n'. Durable fsyncs before returning.
[[nodiscard]] bool append_atomic(Io, const std::filesystem::path& target,
                                 std::string_view bytes,
                                 Durability = Durability::Atomic,
                                 unsigned mode = 0644);

/// append_atomic, plus the byte offset the record landed at, read from the
/// same descriptor after the write (O_APPEND puts it at the end the kernel
/// saw, not the end this process last saw). nullopt on failure. Only valid
/// as an offset while the caller is the file's only writer, which a
/// section guarantees.
[[nodiscard]] std::optional<std::uint64_t> append_record(Io, const std::filesystem::path& target,
                                                         std::string_view bytes,
                                                         Durability = Durability::Atomic);

/// Create `target` holding `bytes` only if it doesn't exist yet. Readers
/// never see it partly written: it's written to a temp, then hard-linked
/// into place, which fails if the name is taken. True if we created it.
[[nodiscard]] bool create_exclusive(Io, const std::filesystem::path& target,
                                    std::string_view bytes, unsigned mode = 0644);

/// The lane for one credential file, shared by everything that writes it
/// (its provider's refresh, an account switch). Ranked cred_file: entered
/// from inside a provider's refresh lane. Lanes are keyed by file name, a
/// small fixed table, so two providers' refreshes rarely share one.
LaneOf<lock_levels::cred_file>& cred_lane(const std::filesystem::path& file);

/// The whole file, or nullopt when it can't be opened. Errors other than
/// "missing" are logged.
[[nodiscard]] std::optional<std::string> read_file(Io, const std::filesystem::path& p);

/// A whole read-modify-write of one small shared file, for files nobody
/// nests with another: read the bytes inside the section, hand them to
/// `edit` (nullopt when absent), and atomically write back what it returns.
/// `edit` returns nullopt to leave the file alone. One process-wide lane
/// (ranked file_lane) serves every such file; the file lock is per path.
///
///     update_shared(io, cap, path, opts, [](std::optional<std::string> old, Args...)
///                                            -> std::optional<std::string> {...}, args...);
enum class Update { Unchanged, Written, Failed };

Lane& small_files_lane();

namespace detail {
// IO from inside a section body: guarded can't carry the caller's Io in.
bool write_in_section(const std::string& target, const std::string& bytes, WriteOptions o);
bool append_in_section(const std::string& target, const std::string& bytes, Durability d,
                       unsigned mode);
bool create_in_section(const std::string& target, const std::string& bytes, unsigned mode);
}  // namespace detail

/// A plain read of a small agentty-owned file (config, trust store) that
/// needs no Io: it changes nothing, and its callers include lock bodies
/// and pure checks. Same result as read_file.
[[nodiscard]] std::optional<std::string> peek_file(const std::filesystem::path& p);

template <class F, class... Args>
Update update_shared(Io, const lock_levels::below_file_lane& cap,
                     const std::filesystem::path& target, WriteOptions o, F edit, Args... args) {
    (void)edit;
    return with_shared_file(cap, small_files_lane(), target,
        [](const maya::held<lock_levels::file_lane>&, Held, std::string t, WriteOptions w,
           Args... a) {
            std::optional<std::string> next = F{}(peek_file(t), std::move(a)...);
            if (!next) return Update::Unchanged;
            return detail::write_in_section(t, *next, w) ? Update::Written : Update::Failed;
        }, target.string(), o, std::move(args)...);
}

}  // namespace agentty::persistence
