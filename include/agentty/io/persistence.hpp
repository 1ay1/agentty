#pragma once
// Filesystem adapter for the store domain.  Lives in `io/` because it
// talks to the filesystem; the concept it satisfies lives in
// `agentty/store/store.hpp`.  Exposed as free functions plus an `FsStore`
// thin wrapper so tests can drop in an alternative without touching
// the rest of the app.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/domain/thread_delta.hpp"
#include "agentty/store/store.hpp"
#include "agentty/util/io.hpp"
#include "agentty/util/lock_levels.hpp"

namespace agentty::persistence {

// Lock order (agentty/util/lock_levels.hpp). Functions that take a lock say
// which, as a capability parameter:
//
//   below_file_lane   enters a shared-file lane (the thread index, the
//                     settings file): call it holding nothing ranked at or
//                     above persistence.file_lane
//   below<leaf>       takes only leaves (the async writer's state)
//   unlocked          blocks on other threads (the flush joins the writer)
//
// A call from inside a lock ranked too high does not compile.

// ── Typed deserialization errors ─────────────────────────────────────────
// The JSON ↔ value-type boundary returns `expected<T, DeserializeError>`
// instead of silently defaulting on missing/invalid fields. The directory-
// walking loader uses this to log + skip bad files rather than dropping
// them silently.
enum class DeserializeErrorKind : std::uint8_t {
    JsonParse,         // body is not valid JSON
    MissingField,      // a required field is absent
    InvalidValue,      // field exists but has wrong type / out of range
    InvalidVariantTag, // unknown discriminator (e.g. status_tag, role)
    Io,                // file-open / read failed
};

struct DeserializeError {
    DeserializeErrorKind kind = DeserializeErrorKind::JsonParse;
    std::string field;        // dotted path to the offending field, "" if N/A
    std::string detail;
    [[nodiscard]] std::string render() const;
};

[[nodiscard]] std::filesystem::path data_dir();
[[nodiscard]] std::filesystem::path threads_dir();

// Write a CLEAN, human/LLM-readable transcript of `t` (role + text, tool
// calls collapsed to a one-line marker — none of the raw JSON noise of the
// on-disk <id>.json). Returns the absolute path written, or empty on
// failure. Used by fork: the new thread starts EMPTY (zero context) and
// the model reads THIS file on demand for earlier context, instead of the
// whole transcript being carried forward.
[[nodiscard]] std::filesystem::path
write_thread_transcript_md(Io, const Thread& t);

// Directory-walking loader: returns every thread we could deserialize,
// **metadata only** (id, title, created_at, updated_at — `messages` is
// empty). The thread picker only needs the metadata; full message bodies
// are loaded lazily via `load_thread_file` on selection. This keeps
// startup RAM proportional to thread count, not transcript bytes — a
// 649-thread / 376 MB on-disk history was loading ~1.2 GB live before.
//
// Files that fail (bad JSON, missing required fields) are logged to
// stderr and skipped — the per-file failure type is `DeserializeError`,
// preserved internally; a caller that wants strict semantics can use
// `load_thread_file` below.
[[nodiscard]] std::vector<Thread> load_all_threads(Io, const lock_levels::below_file_lane&);

// Fill in the picker summary (preview, message count) for threads saved
// before it existed, and store it in the index so it is done once. Returns
// those threads, header only. Slow per thread (a full load), so callers run
// it off the UI thread in small batches, after the list is up.
[[nodiscard]] std::vector<Thread> backfill_picker_summaries(Io, const lock_levels::below_file_lane&,
                                                            const std::vector<ThreadId>& ids);

// Strict per-file loader. Returns the typed error so callers can react
// to specific kinds (e.g. surface MissingField as "schema upgrade needed",
// JsonParse as "corrupt file, restore from backup").
[[nodiscard]] std::expected<Thread, DeserializeError>
load_thread_file(Io, const std::filesystem::path& p);

// Queue a save of the whole thread. The writer still only rewrites what
// changed on disk, but the caller pays for copying every message.
void save_thread(Io, const maya::below<maya::leaf>&, const Thread& t);
// Pure. Fingerprint every persistable message of `t`, compare with `prev`
// (the fps of the last save described for this thread) and copy only the
// messages from the first difference on. An empty `prev` sends everything.
[[nodiscard]] ThreadDelta describe_save(const Thread& t, const std::vector<std::uint64_t>& prev);
// Pure. Every field of `t` but its messages, plus the picker summary.
[[nodiscard]] Thread thread_header(const Thread& t);
// Queue a described save. Returns false (nothing queued) when the delta has
// no base: an earlier save of this thread failed or was never seen. The
// caller then sends the whole thread (describe_save with an empty `prev`).
// A delta with from == 0 is a whole thread and is always queued.
[[nodiscard]] bool save_thread_delta(Io, const maya::below<maya::leaf>&, ThreadDelta d);
// Block until every queued background save has hit disk. Call at
// shutdown after the last reducer step (Quit handler issues a final
// save_thread) so we don't lose the most-recent transcript on exit. It
// joins the writer, so it needs `unlocked`.
void flush_pending_saves(Io, const maya::unlocked&);
void delete_thread(Io, const lock_levels::below_thread_log&, const ThreadId& id);
// Retitle a saved thread. Queued on the same writer as save_thread, so it
// lands after any write of that thread already in flight, and a save still
// queued takes the new title instead of writing the old one back over it.
// updated_at is kept, so the thread does not move in the picker. `title`
// is stored as given: clean it with clean_title() first. `renamed_at`
// (unix seconds) is stored with it; a later save with an older stamp
// keeps this title instead of its own. `name_is_auto` marks a cleared rename:
// the stamp still wins over stale copies, but the thread may be auto-titled.
void rename_thread(Io, const maya::below<maya::leaf>&, const ThreadId& id, std::string title,
                   std::int64_t renamed_at, bool name_is_auto = false);

// Thread retention. Deletes threads with no activity (file mtime) in the
// last `keep_days` days, skipping `keep` (the open one). keep_days <= 0
// does nothing. Their blobs are left for blob gc. Returns what it deleted
// (or would, with dry_run).
struct ExpireStats { std::size_t threads = 0; std::uintmax_t bytes = 0; };
ExpireStats expire_threads(Io, const lock_levels::below_thread_log&, int keep_days,
                           const ThreadId& keep, bool dry_run = false);

// $AGENTTY_THREADS_KEEP_DAYS, or 0 (keep everything) when unset or invalid.
[[nodiscard]] int thread_keep_days(Io);

// The per-message fingerprint the incremental save path uses to find what
// changed. Exposed ONLY so the save bench can price that pass separately
// from the rest of a save; nothing in the app should call it.
[[nodiscard]] std::uint64_t debug_message_fingerprint(const Message& m);
// Test hook: make the writer forget what it knows about `id`, as a failed
// save does. Nothing in the app should call it.
void debug_forget_saved(const maya::below<maya::leaf>&, const ThreadId& id);

[[nodiscard]] store::Settings load_settings(Io);
// Write settings.json, then tell the settings cache (on_settings_written) so
// it drops its copy. The cache's reaction drains its queue -- a wait -- so
// this needs `unlocked`.
void save_settings(Io, const maya::unlocked&, const store::Settings& s);
// The same write WITHOUT telling the cache: for the cache itself, whose own
// write must not invalidate it. Takes only the settings lane.
void save_settings_quiet(Io, const lock_levels::below_file_lane&, const store::Settings& s);

// Called after every save_settings() that goes DIRECTLY to disk.
//
// The TUI reads settings through a write-behind cache
// (runtime/app/settings_cache.hpp) that assumes it is the only writer. It
// isn't: the credential helpers (credentials::add_key, vault::key_clear,
// account_switch, the --model/--provider CLI paths) call save_settings here
// directly, because they also run on paths where no Deps exists. A cache
// holding a copy from before such a write will publish it back on the next
// save and silently undo it.
//
// So the cache registers itself here and drops its copy. This lives at the
// io layer rather than in each writer because the writers span provider/,
// io/ and main() — and a new one would otherwise have to KNOW to invalidate,
// which is exactly the kind of rule that gets forgotten. The observer is
// handed the writer's `unlocked`: it may wait (drain), and the type says so.
void on_settings_written(std::function<void(const maya::unlocked&)> observer);

[[nodiscard]] ThreadId new_id();
// 64 random bits as 16 hex digits; backs new_id() and new_message_id().
[[nodiscard]] std::string random_hex_id();
[[nodiscard]] std::string title_from_first_message(std::string_view text);

// The longest title, in bytes: what title_from_first_message produces and
// what a rename is held to, so every picker row has the same budget.
inline constexpr std::size_t kTitleMaxBytes = 60;

// A typed or pasted title made safe to store and paint on one row: line
// breaks and tabs become spaces, other control bytes are dropped (a title
// reaches the terminal, so no escape sequences), invalid UTF-8 is repaired,
// the result is cut to `max_bytes` on a code-point boundary and trimmed
// of blanks at both ends. May return "".
[[nodiscard]] std::string clean_title(std::string_view text,
                                      std::size_t max_bytes = kTitleMaxBytes);

// The picker preview: the first message the user typed, made one safe line
// the same way a title is.
inline constexpr std::size_t kPreviewMaxBytes = 200;

// Fill out.preview and out.message_count from `messages`. Does nothing for
// an empty list, so a header-only reload can't blank them.
void summarise_for_picker(const std::vector<Message>& messages, Thread& out);
// The same, reading t.messages.
void refresh_picker_summary(Thread& t);

// Atomic + durable write of `content` to `target` (persistence::write_atomic
// with Durability::Durable): a unique temp, fsync, rename. Readers see the old
// file or the new one. False on any I/O failure (the temp is removed).
bool write_json_atomic(Io, const std::filesystem::path& target,
                       const std::string& content);

// ── Per-message codec ────────────────────────────────────────────────
//
// The single definition of "a Message on disk". Exposed because the
// thread LOG (io/thread_log.hpp) stores one of these per line and must
// produce byte-identical output to the whole-document writer — two
// codecs would drift, and the drift would only show up as silently
// mangled history in whichever format was written less often.
//
// Both sides carry real behaviour worth not duplicating: the writer
// scrubs to valid UTF-8 (arbitrary tool output can hold Latin-1 or
// Shift-JIS bytes that nlohmann refuses to dump) and lifts large
// payloads into the blob store; the reader accepts every historical
// shape, including inline payloads written before blobs existed.
[[nodiscard]] nlohmann::json message_to_json(const Message& m);
[[nodiscard]] std::expected<Message, DeserializeError>
message_from_json(const nlohmann::json& j);

// The same, for everything a Thread carries EXCEPT its messages: title,
// fork provenance, RAG override, timestamps, compaction records.
//
// Split out because the two halves have different lifetimes on disk. The
// messages are append-only; the metadata is mutable and rewritten whole.
// The thread log stores them in separate files for exactly that reason,
// and reuses these so there is still ONE definition of each field's
// on-disk form.
//
// `thread_meta_to_json` never emits a "messages" key;
// `thread_meta_from_json` ignores one if present, so it can also be
// pointed at a legacy whole-thread document to read just the header.
[[nodiscard]] nlohmann::json thread_meta_to_json(const Thread& t);
[[nodiscard]] std::expected<Thread, DeserializeError>
thread_meta_from_json(const nlohmann::json& j);

// Drop compaction records whose boundary lies past the end of
// `t.messages`. `thread_meta_from_json` cannot do this itself — in the
// log format the metadata is read before any message is — so the caller
// applies it once the transcript is attached. A stale record survives
// only an interrupted save, and would otherwise make the wire summarise
// a prefix that no longer exists.
void clamp_compactions(Thread& t);

// Load one thread by id, preferring the LOG format and falling back to
// the legacy whole-document `<id>.json`.
//
// This is the seam where the new format becomes load-bearing. Both
// formats are read forever: a user's history predates the log, and
// nothing is rewritten until that thread is next saved.
[[nodiscard]] std::optional<Thread> load_thread_by_id(Io, const ThreadId& id);

// Subagent runs kept for an editor to open (threads/subagents/<id>.json).
// Not in the thread list; the blob GC reads them.
bool save_subagent_thread(Io, const Thread& t);
[[nodiscard]] std::optional<Thread> load_subagent_thread(Io, const ThreadId& id);

} // namespace agentty::persistence

namespace agentty::io {

// Filesystem-backed store satisfying agentty::store::Store. Reached only
// through app::deps(Io), so it holds the Io the host gave it.
//
// Lock order: the store::Store concept has no capability in its signatures,
// because the store is an EFFECT seam -- it is only ever called by the
// effect host (app::Host) and by deps(), which the host and init reach,
// both on the loop thread between steps, where nothing is held. That is the
// same promise its Io makes ("I am an effect entry point"); each call here
// re-states it as a capability root, one place, reviewed (lint: io_roots +
// cap_roots list this file).
class FsStore {
public:
    explicit FsStore(Io io) : io_(io) {}
    [[nodiscard]] std::vector<Thread> load_threads() {
        return maya::thread_root(
            [&](const maya::unlocked& c) { return persistence::load_all_threads(io_, c); });
    }
    [[nodiscard]] std::optional<Thread> load_thread(const ThreadId& id) {
        return persistence::load_thread_by_id(io_, id);
    }
    [[nodiscard]] std::vector<Thread> backfill_summaries(const std::vector<ThreadId>& ids) {
        return maya::thread_root([&](const maya::unlocked& c) {
            return persistence::backfill_picker_summaries(io_, c, ids);
        });
    }
    void save_thread(const Thread& t) {
        maya::thread_root([&](const maya::unlocked& c) { persistence::save_thread(io_, c, t); });
    }
    void delete_thread(const ThreadId& id) {
        maya::thread_root([&](const maya::unlocked& c) { persistence::delete_thread(io_, c, id); });
    }
    [[nodiscard]] store::Settings load_settings()          { return persistence::load_settings(io_); }
    void save_settings(const store::Settings& s) {
        maya::thread_root([&](const maya::unlocked& c) { persistence::save_settings(io_, c, s); });
    }
    // The settings cache's own disk write: doesn't notify the cache.
    void save_settings_quiet(const maya::unlocked& c, const store::Settings& s) {
        persistence::save_settings_quiet(io_, c, s);
    }
    [[nodiscard]] ThreadId new_id()              { return persistence::new_id(); }
    [[nodiscard]] std::string title_from(std::string_view text) {
        return persistence::title_from_first_message(text);
    }

private:
    Io io_;
};

static_assert(store::Store<FsStore>);

} // namespace agentty::io
