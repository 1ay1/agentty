#pragma once
// Filesystem adapter for the store domain.  Lives in `io/` because it
// talks to the filesystem; the concept it satisfies lives in
// `agentty/store/store.hpp`.  Exposed as free functions plus an `FsStore`
// thin wrapper so tests can drop in an alternative without touching
// the rest of the app.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/conversation.hpp"
#include "agentty/store/store.hpp"

namespace agentty::persistence {

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
[[nodiscard]] std::vector<Thread> load_all_threads();

// Strict per-file loader. Returns the typed error so callers can react
// to specific kinds (e.g. surface MissingField as "schema upgrade needed",
// JsonParse as "corrupt file, restore from backup").
[[nodiscard]] std::expected<Thread, DeserializeError>
load_thread_file(const std::filesystem::path& p);

void save_thread(const Thread& t);
void delete_thread(const ThreadId& id);

[[nodiscard]] store::Settings load_settings();
void save_settings(const store::Settings& s);

[[nodiscard]] ThreadId new_id();
[[nodiscard]] std::string title_from_first_message(std::string_view text);

} // namespace agentty::persistence

namespace agentty::io {

// Filesystem-backed store satisfying agentty::store::Store.
class FsStore {
public:
    [[nodiscard]] std::vector<Thread> load_threads() {
        return persistence::load_all_threads();
    }
    [[nodiscard]] std::optional<Thread> load_thread(const ThreadId& id) {
        auto p = persistence::threads_dir() / (id.value + ".json");
        auto loaded = persistence::load_thread_file(p);
        if (!loaded) return std::nullopt;
        return std::move(*loaded);
    }
    void save_thread(const Thread& t)            { persistence::save_thread(t); }
    [[nodiscard]] store::Settings load_settings()          { return persistence::load_settings(); }
    void save_settings(const store::Settings& s) { persistence::save_settings(s); }
    [[nodiscard]] ThreadId new_id()              { return persistence::new_id(); }
    [[nodiscard]] std::string title_from(std::string_view text) {
        return persistence::title_from_first_message(text);
    }
};

static_assert(store::Store<FsStore>);

} // namespace agentty::io
