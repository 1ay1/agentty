// agentty::tools::memory — implementation. See memory_store.hpp for the
// shape of the data and the storage rationale.
//
// On-disk format (JSONL, one record per line):
//
//   {"id":"a1b2c3d4","ts":1731860000,"scope":"project","text":"prefer fish"}
//
// Reads tolerate slop: lines that fail to parse are skipped and counted
// internally for a future "memory health" surface; the agent never sees
// them. Writes are append-only on the happy path; the rare cap-rollover
// path rewrites the file atomically through util::write_file (which
// itself uses write+flush+rename when the underlying impl supports it).

#include "agentty/tool/memory_store.hpp"

#include "agentty/tool/util/fs_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace agentty::tools::memory {

namespace {

using json = nlohmann::json;

// Process-wide mutex serialising every read-modify-write on either
// scope file. The tool dispatcher already runs each tool call on a
// dedicated worker thread, so contention is at most "the agent fired
// remember + forget back-to-back" — fine to serialise on a single mu.
std::mutex& store_mu() {
    static std::mutex m;
    return m;
}

[[nodiscard]] fs::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
#endif
    return {};
}

// Generate an 8-char hex id. `random_device` is seeded once per
// process; collisions across the lifetime of one memory file are
// negligibly rare (2^32 space, ~hundreds of records). Two records
// landing on the same id within milliseconds of each other would just
// produce two lines with the same id — `forget {id}` would clear
// both, which is the user-intuitive outcome anyway.
[[nodiscard]] std::string make_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex rng_mu;
    std::uint32_t v;
    {
        std::lock_guard lk(rng_mu);
        v = static_cast<std::uint32_t>(rng());
    }
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return std::string{buf, 8};
}

[[nodiscard]] std::int64_t now_unix() noexcept {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Trim ASCII whitespace from both ends. We deliberately don't normalise
// internal whitespace or NFC — what the user typed is what the model
// reads back later.
[[nodiscard]] std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string{s.substr(b, e - b)};
}

[[nodiscard]] std::optional<Record> parse_record_line(std::string_view line) {
    std::string s = trim(line);
    if (s.empty()) return std::nullopt;
    try {
        auto j = json::parse(s);
        if (!j.is_object()) return std::nullopt;
        Record r;
        r.id   = j.value("id", std::string{});
        r.ts   = j.value("ts", std::int64_t{0});
        auto sc = parse_scope(j.value("scope", std::string{}));
        if (!sc) return std::nullopt;
        r.scope = *sc;
        r.text = j.value("text", std::string{});
        if (r.id.empty() || r.text.empty()) return std::nullopt;
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::string serialise_record(const Record& r) {
    json j;
    j["id"]    = r.id;
    j["ts"]    = r.ts;
    j["scope"] = to_string(r.scope);
    j["text"]  = r.text;
    // dump() with no indent: one record per line by construction.
    return j.dump();
}

[[nodiscard]] std::vector<Record> read_records(const fs::path& p) {
    std::vector<Record> out;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (auto r = parse_record_line(line)) out.push_back(std::move(*r));
    }
    return out;
}

// Atomic-ish rewrite: write to a sibling .tmp, fsync, rename over. On
// platforms where util::write_file already does this, we get it for
// free; on simpler platforms it's still strictly safer than truncating
// the live file. Returns empty string on success.
[[nodiscard]] std::string write_records(const fs::path& p,
                                        const std::vector<Record>& recs) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // Ignore ec — write_file will surface the real error.
    std::ostringstream out;
    for (const auto& r : recs) out << serialise_record(r) << '\n';
    return util::write_file(p, out.str());
}

// Append one already-serialised line. Uses an actual O_APPEND open so
// concurrent writers don't clobber each other at the byte level. If
// the open fails we fall back to the slow path (read all + write all)
// so a missing parent directory still gets handled. Returns empty
// string on success.
[[nodiscard]] std::string append_line(const fs::path& p, std::string_view line) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (f) {
            f.write(line.data(), static_cast<std::streamsize>(line.size()));
            f.put('\n');
            if (f.good()) return {};
        }
    }
    // Slow path — recreate by reading-and-writing.
    auto recs = read_records(p);
    if (auto r = parse_record_line(line)) recs.push_back(std::move(*r));
    return write_records(p, recs);
}

// ── mtime-keyed cache for the prompt-loader hot path ────────────────────
// `default_system_prompt` calls load_recent_*() on every turn. Mirrors
// the read_memory_cached pattern in transport.cpp so memory loading
// stays free when nothing has changed on disk. The cache key is the
// scope-file path; the value is the tail-N records list, regenerated
// when mtime moves.
struct CacheEntry {
    fs::file_time_type mtime{};
    std::vector<Record> tail;
};
std::unordered_map<std::string, CacheEntry>& cache() {
    static std::unordered_map<std::string, CacheEntry> c;
    return c;
}
std::mutex& cache_mu() {
    static std::mutex m;
    return m;
}

[[nodiscard]] std::vector<Record> tail_of(const std::vector<Record>& all,
                                          std::size_t n) {
    if (all.size() <= n) return all;
    return std::vector<Record>(all.end() - static_cast<std::ptrdiff_t>(n),
                               all.end());
}

[[nodiscard]] std::vector<Record> load_recent_for(Scope s) {
    const auto p = path_for(s);
    if (p.empty()) return {};
    const auto key = p.string();
    std::error_code ec;
    auto now_mt = fs::last_write_time(p, ec);
    if (ec) {
        // Missing file — drop cache entry so a future re-creation is observed.
        std::lock_guard lk(cache_mu());
        cache().erase(key);
        return {};
    }
    {
        std::lock_guard lk(cache_mu());
        auto it = cache().find(key);
        if (it != cache().end() && it->second.mtime == now_mt) return it->second.tail;
    }
    auto all = read_records(p);
    auto tail = tail_of(all, kTailLoadCount);
    {
        std::lock_guard lk(cache_mu());
        cache()[key] = CacheEntry{now_mt, tail};
    }
    return tail;
}

// Invalidate the cache entry for one scope after a mutating call.
// `last_write_time` is updated by the OS when we write, so the cache
// would self-heal on the next stat — but invalidating eagerly keeps
// the next loader call from racing with filesystem time granularity
// (HFS+ and some NFS exports have second-resolution mtimes).
void bump_cache(Scope s) {
    const auto p = path_for(s);
    if (p.empty()) return;
    std::lock_guard lk(cache_mu());
    cache().erase(p.string());
}

} // namespace

std::optional<Scope> parse_scope(std::string_view s) noexcept {
    if (s == "user") return Scope::User;
    if (s == "project") return Scope::Project;
    return std::nullopt;
}

fs::path path_for(Scope s) {
    if (s == Scope::User) {
        auto h = home_dir();
        if (h.empty()) return {};
        return h / ".agentty" / "memory.jsonl";
    }
    // Scope::Project — anchored on the workspace root so subprocess
    // calls that cd around don't shift where memory lives.
    const auto& root = util::workspace_root();
    if (root.empty()) return {};
    return root / ".agentty" / "memory.jsonl";
}

AppendResult append(Scope s, std::string_view text) {
    AppendResult res;
    std::string body = trim(text);
    if (body.empty()) {
        res.error = "remember: text is empty after trim";
        return res;
    }
    if (body.size() > kMaxTextBytes) {
        // Truncate on a (likely-)UTF-8-safe boundary. We don't pull in
        // the full utf8 helper here — back off to the last leading byte
        // within the cap. The 4-byte rewind covers the longest legal
        // UTF-8 sequence; worst case we lose one trailing code point.
        std::size_t cut = kMaxTextBytes;
        for (int back = 0; back < 4 && cut > 0; ++back, --cut) {
            unsigned char c = static_cast<unsigned char>(body[cut]);
            if ((c & 0xC0) != 0x80) break;
        }
        res.note = "text truncated to " + std::to_string(cut) + " bytes (was "
                 + std::to_string(body.size()) + ")";
        body.resize(cut);
    }

    Record r;
    r.id    = make_id();
    r.ts    = now_unix();
    r.scope = s;
    r.text  = std::move(body);

    const auto p = path_for(s);
    if (p.empty()) {
        res.error = "remember: can't resolve "
                  + std::string{to_string(s)}
                  + " memory path (HOME / workspace_root unset)";
        return res;
    }

    std::lock_guard lk(store_mu());

    // Cap enforcement: load + tail-cap + append + cap-by-bytes, then
    // either append-line (fast path, no rollover) or full rewrite.
    auto existing = read_records(p);
    bool rollover = false;
    // Record-count cap.
    if (existing.size() + 1 > kMaxRecordsPerScope) {
        std::size_t drop = existing.size() + 1 - kMaxRecordsPerScope;
        existing.erase(existing.begin(),
                       existing.begin() + static_cast<std::ptrdiff_t>(drop));
        res.rolled += drop;
        rollover = true;
    }
    existing.push_back(r);
    // Byte cap — drop oldest until we fit. Each record's serialised
    // form is bounded by kMaxTextBytes + ~64 bytes of envelope, so
    // worst-case we drop a handful.
    auto total_bytes = [](const std::vector<Record>& rs) {
        std::size_t b = 0;
        for (const auto& x : rs) b += serialise_record(x).size() + 1;
        return b;
    };
    while (existing.size() > 1 && total_bytes(existing) > kMaxFileBytes) {
        existing.erase(existing.begin());
        ++res.rolled;
        rollover = true;
    }

    std::string err;
    if (rollover) {
        err = write_records(p, existing);
    } else {
        err = append_line(p, serialise_record(r));
    }
    if (!err.empty()) {
        res.error = "remember: " + err;
        return res;
    }
    res.id = r.id;
    bump_cache(s);
    return res;
}

std::vector<Record> load_all(Scope s) {
    const auto p = path_for(s);
    if (p.empty()) return {};
    std::lock_guard lk(store_mu());
    return read_records(p);
}

std::vector<Record> load_recent_user()    { return load_recent_for(Scope::User); }
std::vector<Record> load_recent_project() { return load_recent_for(Scope::Project); }

std::size_t forget_by_id(std::string_view id) {
    std::string want{id};
    if (trim(want).empty()) return 0;
    std::lock_guard lk(store_mu());
    std::size_t removed = 0;
    for (auto s : {Scope::User, Scope::Project}) {
        const auto p = path_for(s);
        if (p.empty()) continue;
        auto recs = read_records(p);
        std::size_t before = recs.size();
        recs.erase(std::remove_if(recs.begin(), recs.end(),
                                  [&](const Record& r){ return r.id == want; }),
                   recs.end());
        if (recs.size() != before) {
            (void)write_records(p, recs);
            removed += before - recs.size();
            bump_cache(s);
        }
    }
    return removed;
}

std::size_t forget_by_substring(std::string_view needle) {
    std::string want = trim(needle);
    if (want.empty()) return 0;
    std::lock_guard lk(store_mu());
    std::size_t removed = 0;
    for (auto s : {Scope::User, Scope::Project}) {
        const auto p = path_for(s);
        if (p.empty()) continue;
        auto recs = read_records(p);
        std::size_t before = recs.size();
        recs.erase(std::remove_if(recs.begin(), recs.end(),
                                  [&](const Record& r){
                                      return r.text.find(want) != std::string::npos;
                                  }),
                   recs.end());
        if (recs.size() != before) {
            (void)write_records(p, recs);
            removed += before - recs.size();
            bump_cache(s);
        }
    }
    return removed;
}

std::string render_for_prompt(const Record& r) {
    std::string out;
    out.reserve(r.text.size() + 16);
    out += '[';
    out += r.id;
    out += "] ";
    out += r.text;
    return out;
}

} // namespace agentty::tools::memory
