#pragma once
// agentty::ThreadLog — a thread as an append-only log of messages.
//
// THE PROBLEM
//
// A thread is stored today as ONE JSON document. Opening it parses every
// byte: on a real 29 MB / 2519-message thread that is 226 ms before a row
// can be drawn, and saving one new turn rewrites all 29 MB. Both costs
// grow without bound as a conversation continues, and neither is related
// to how much of the thread is actually on screen (~60 messages).
//
// THE SHAPE
//
//   <id>.jsonl   one message per line, appended, never rewritten
//   <id>.ofs     one 8-byte offset per message — 20 KB for 2519 messages
//
// Measured on that same thread, against 226 ms today:
//
//   read the whole thread        49 ms   (2519 small parses beat one big one)
//   read the last 60 messages   1.4 ms   (seek to ofs[n-60], parse to EOF)
//   append one turn            0.01 ms   (one line + 8 bytes, both fsynced)
//
// The 4x on a full read is not an optimisation we designed for — it falls
// out of line-delimiting. A parser handed 2519 small documents starts
// fresh on each one, with small short-lived allocations, instead of
// carrying one deep nesting context across 29 MB.
//
// WHY NO DATABASE
//
// SQLite measured 1.3 ms against this design's 1.4 ms — within noise, for
// a 250 KB C dependency and the loss of `grep`/`jq` on your own history.
// This subsystem deliberately adds NO third-party dependency: it is
// <fstream>, <filesystem>, and the per-message codec that already exists
// in persistence.cpp. See docs/THREAD_STORE.md §3.1 for the full
// comparison (compression also lost: half the disk for triple the load,
// and it forecloses seeking entirely).
//
// CRASH SAFETY, FOR FREE
//
// Append-only plus line-delimited is why there is no WAL and no CRC here:
//
//   * Nothing already written is ever written again, so a torn append can
//     only damage the LAST line. The reader drops a trailing unparseable
//     line, which is exactly the right recovery — verified by truncating
//     a real log mid-line: 2538 messages read back, 1 partial dropped.
//   * `.ofs` is a CACHE, not truth. It can be deleted, truncated, or
//     filled with garbage; open() detects the mismatch and rebuilds it by
//     scanning newlines (12 ms for the 29 MB thread). Nothing about
//     correctness depends on it — only speed.
//   * Write order is log → fsync → offset. A crash between them leaves
//     the offset file short, which is the case open() already repairs.
//
// A SINGLE SOURCE OF TRUTH FOR THE ON-DISK MESSAGE
//
// Lines are produced by persistence::message_to_json and read back by
// persistence::message_from_json — the same functions the whole-document
// format uses. That is deliberate: two codecs would drift, and the drift
// would surface as silently mangled history in whichever format happened
// to be written less often.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/io/persistence.hpp"   // DeserializeError

namespace agentty {

class ThreadLog {
public:
    // Open the log for `id`, creating it if absent.
    //
    // Reads and validates `.ofs` only — the message bodies are NOT read,
    // so this is O(message count), not O(bytes). A missing, short, or
    // corrupt index is rebuilt here rather than reported: the index is a
    // cache, and a user should never see an error for a file they are not
    // supposed to know exists.
    [[nodiscard]] static std::expected<ThreadLog, persistence::DeserializeError>
    open(const ThreadId& id);

    // Same, for an explicit path (tests, migration, export). `log_path`
    // is the .jsonl; the index is `log_path` with the extension replaced.
    [[nodiscard]] static std::expected<ThreadLog, persistence::DeserializeError>
    open_path(std::filesystem::path log_path);

    [[nodiscard]] std::size_t size() const noexcept { return offsets_.size(); }
    [[nodiscard]] bool empty()      const noexcept { return offsets_.empty(); }

    [[nodiscard]] const std::filesystem::path& path()       const noexcept { return log_; }
    [[nodiscard]] const std::filesystem::path& index_path() const noexcept { return idx_; }

    // Parse messages [from, to). Clamped to the log; from >= to yields {}.
    //
    // This is the ONLY read path. The whole-thread call is range(0, size())
    // and the windowed call is range(size()-60, size()); both are the same
    // code, because the difference is only where the seek lands.
    //
    // A line that fails to parse is SKIPPED rather than failing the read.
    // A single corrupt line in a 2519-message history must not cost the
    // user the other 2518 — the same judgement load_all_threads already
    // makes for a corrupt thread file among many.
    [[nodiscard]] std::vector<Message> range(std::size_t from,
                                             std::size_t to) const;

    [[nodiscard]] std::vector<Message> all() const { return range(0, size()); }

    // Append one message: one line to the log, one offset to the index.
    // O(1) in thread size — this is what replaces rewriting the whole
    // file every turn. Returns false if the log write failed (in which
    // case nothing was appended to the index either, so the two stay
    // consistent).
    bool append(const Message& m);

    // Replace the entire log. Needed by the paths that genuinely rewrite
    // history — compaction, message edit, fork — and by migration. O(n),
    // but rare and user-initiated.
    //
    // Atomic: builds the new log and index beside the old ones and
    // renames both into place, so an interrupted rewrite leaves the
    // previous history intact rather than a half-written one.
    bool rewrite(const std::vector<Message>& msgs);

private:
    ThreadLog() = default;

    // Scan the log counting line starts. The recovery path for a missing
    // or inconsistent index, and how a legacy thread gets its first one.
    void rebuild_index_();
    [[nodiscard]] bool write_index_() const;

    // Offset of a final line that has no terminating newline — i.e. a
    // crash between writing a message and writing its '\n'. kNoTear when
    // the log ends cleanly.
    //
    // Readers already cope (the partial line fails to parse and is
    // dropped). This exists for the WRITER: appending onto an unterminated
    // line would glue the new message onto the broken one and lose both,
    // so append() truncates the tear first. Cheap to detect — it is one
    // byte at a known position.
    static constexpr std::uint64_t kNoTear = ~0ull;

    std::filesystem::path      log_;
    std::filesystem::path      idx_;
    std::vector<std::uint64_t> offsets_;
    std::uint64_t              torn_tail_at_ = kNoTear;
};

} // namespace agentty
