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
#include "agentty/tool/util/utf8.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#  include <pwd.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

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
#else
    // HOME can legitimately be unset (cron, systemd units with
    // ProtectHome=, containers launched without --env HOME). Fall
    // back to the password database for the current uid — every
    // POSIX system has a real home for every login uid.
    if (::geteuid() != 0 || std::getenv("SUDO_USER") == nullptr) {
        // getpwuid_r is the thread-safe variant; bound the buffer at
        // 16 KiB, more than any sane pw_dir + login name + shell.
        std::vector<char> buf(4096);
        struct passwd  pw{};
        struct passwd* result = nullptr;
        for (;;) {
            int e = ::getpwuid_r(::geteuid(), &pw, buf.data(), buf.size(), &result);
            if (e == 0) break;
            if (e == ERANGE && buf.size() < (1u << 16)) {
                buf.resize(buf.size() * 2);
                continue;
            }
            result = nullptr;
            break;
        }
        if (result && result->pw_dir && *result->pw_dir)
            return fs::path{result->pw_dir};
    }
#endif
    return {};
}

// True if `dir` already exists and is writable by us, OR doesn't exist
// yet but its first existing ancestor is writable (so create_directories
// will succeed). False on root='/' for any non-root user, on a path
// inside a read-only mount, etc. The check is best-effort — a TOCTOU
// race between this and the actual write is harmless: the writer's own
// errno surfaces if the race lands the wrong way.
[[nodiscard]] bool dir_path_writable(const fs::path& dir) noexcept {
#ifdef _WIN32
    // ::access(W_OK) on Windows is famously unreliable for directory
    // ACL checks. Defer to the write itself — return true and let the
    // failure path produce the real Windows error message. The fall-
    // back logic below is only there for POSIX hosts that mount /
    // read-only or run as nobody.
    (void)dir;
    return true;
#else
    std::error_code ec;
    fs::path probe = dir;
    while (!probe.empty() && !fs::exists(probe, ec)) {
        auto parent = probe.parent_path();
        if (parent == probe) break;            // can't ascend past root
        probe = parent;
    }
    if (probe.empty()) return false;
    return ::access(probe.c_str(), W_OK) == 0;
#endif
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
        // Optional fields — absent in legacy records, defaulted on read.
        // Defensive parses: a hand-edited file with the wrong type for
        // a field falls back to the default rather than dropping the
        // whole record.
        if (j.contains("pinned") && j["pinned"].is_boolean())
            r.pinned = j["pinned"].get<bool>();
        if (j.contains("hits") && j["hits"].is_number_integer())
            r.hits = j["hits"].get<std::int32_t>();
        if (j.contains("tags") && j["tags"].is_array()) {
            for (const auto& t : j["tags"]) {
                if (!t.is_string()) continue;
                auto s2 = t.get<std::string>();
                if (!s2.empty()) r.tags.push_back(std::move(s2));
            }
        }
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
    // Only emit optional fields when set — keeps the line short for the
    // common case (no tags, not pinned, never deduped) and the on-disk
    // format byte-identical to the legacy shape.
    if (r.pinned)        j["pinned"] = true;
    if (r.hits > 0)      j["hits"]   = r.hits;
    if (!r.tags.empty()) j["tags"]   = r.tags;
    // dump() with no indent: one record per line by construction.
    return j.dump();
}

// Lower-case ASCII + collapse internal whitespace runs to single space
// + strip ends. Used by the dedup hash so "Build moha with cmake "
// matches "build moha with cmake". Does NOT touch UTF-8 multi-byte
// runs — the model is unlikely to vary case on those between calls and
// folding them properly would pull in ICU.
[[nodiscard]] std::string normalise_for_dedup(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool in_ws = true;   // skip leading whitespace
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!in_ws) { out.push_back(' '); in_ws = true; }
            continue;
        }
        in_ws = false;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Cheap similarity: prefix-bounded Jaro for short strings. Returns a
// score in [0, 1]; we treat ≥ 0.92 as "same fact, different wording".
// We don't pull in Jaro-Winkler because the prefix bias would make
// "build with cmake" and "build with cargo" score >0.95 — they share a
// common prefix but mean different things. Plain Jaro is enough
// resolution for the typical model-restatement case.
[[nodiscard]] double jaro_similarity(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    const std::size_t la = a.size();
    const std::size_t lb = b.size();
    const std::size_t match_window = la > lb ? la / 2 : lb / 2;
    std::vector<char> a_matched(la, 0);
    std::vector<char> b_matched(lb, 0);
    std::size_t matches = 0;
    for (std::size_t i = 0; i < la; ++i) {
        const std::size_t lo = i > match_window ? i - match_window : 0;
        const std::size_t hi = std::min(i + match_window + 1, lb);
        for (std::size_t j = lo; j < hi; ++j) {
            if (b_matched[j]) continue;
            if (a[i] != b[j])  continue;
            a_matched[i] = 1;
            b_matched[j] = 1;
            ++matches;
            break;
        }
    }
    if (matches == 0) return 0.0;
    // Transpositions: count pairs that match but in different order.
    std::size_t k = 0, t = 0;
    for (std::size_t i = 0; i < la; ++i) {
        if (!a_matched[i]) continue;
        while (!b_matched[k]) ++k;
        if (a[i] != b[k]) ++t;
        ++k;
    }
    const double m = static_cast<double>(matches);
    return (m / la + m / lb + (m - t / 2.0) / m) / 3.0;
}

// Tag normalisation: lower-case ASCII, drop empties, dedup, sort.
[[nodiscard]] std::vector<std::string> normalise_tags(
        const std::vector<std::string>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (auto t : in) {
        // lowercase ASCII in place
        for (auto& c : t)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        // trim
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
            t.erase(t.begin());
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
            t.pop_back();
        if (!t.empty()) out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
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
    //
    // Degenerate-root guard: if workspace_root() couldn't resolve a
    // real cwd at startup it falls back to fs::path{"/"}, and a
    // remember call would then try to mkdir /.agentty — fails with
    // EACCES for any non-root user and surfaces the unhelpful
    // "failed to create directory '/.agentty': Permission denied"
    // error. Treat root=="/" and other unwritable roots as if no
    // project scope is available. append() then REFUSES the call with a
    // guiding error rather than silently writing the fact to user scope —
    // a silent promotion would leak a project fact into every OTHER
    // workspace's system prompt (cross-workspace "memory bleed").
    const auto& root = util::workspace_root();
    if (root.empty()) return {};
    if (root == fs::path{"/"}) return {};
    if (!dir_path_writable(root / ".agentty")) return {};
    return root / ".agentty" / "memory.jsonl";
}

AppendResult append(Scope s, std::string_view text, AppendOptions opts) {
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

    auto tags_norm = normalise_tags(opts.tags);

    auto p = path_for(s);
    Scope actual_scope = s;
    if (p.empty() && s == Scope::Project) {
        // Project scope unavailable (workspace root == "/", unwritable, or
        // unset). Do NOT silently fall back to user scope: a project fact
        // promoted to user scope loads into EVERY other workspace's system
        // prompt — the cross-workspace "memory bleed" where a fact about
        // project A surfaces while you're working on project B. Refuse, and
        // tell the caller how to opt in to a global fact explicitly.
        res.error =
            "remember: project scope is unavailable here — workspace root '"
            + util::workspace_root().string()
            + "' is not writable, so there is nowhere to store a project-"
              "scoped fact. Re-run with scope=\"user\" if you intended a "
              "global fact (it will load into every workspace), or start "
              "agentty in a writable project directory.";
        return res;
    }
    if (p.empty()) {
        // s == Scope::User and even the user path is unresolvable.
        res.error = "remember: can't resolve a writable user memory path "
                    "(HOME is unset and getpwuid_r returned no home directory).";
        return res;
    }

    std::lock_guard lk(store_mu());
    auto existing = read_records(p);

    // ── Dedup pass ───────────────────────────────────────────────────
    // Walk existing records looking for one whose normalised text is
    // a near-match for the incoming body. On hit: refresh ts, bump
    // hits, merge pin ("once pinned, stays pinned"), union tags.
    // No new id is allocated; the existing record's id is returned
    // and `deduped` flags the dedup for the caller.
    if (!opts.no_dedup) {
        const auto needle = normalise_for_dedup(body);
        for (auto& rec : existing) {
            if (rec.scope != actual_scope) continue;
            const auto hay = normalise_for_dedup(rec.text);
            if (hay == needle
                || jaro_similarity(hay, needle) >= 0.92) {
                rec.ts = now_unix();
                if (rec.hits < std::numeric_limits<std::int32_t>::max())
                    ++rec.hits;
                if (opts.pinned) rec.pinned = true;
                if (!tags_norm.empty()) {
                    auto merged = rec.tags;
                    merged.insert(merged.end(),
                                  tags_norm.begin(), tags_norm.end());
                    rec.tags = normalise_tags(merged);
                }
                // Supersede during dedup: rare but possible (a model
                // both restates and tries to drop a prior version).
                // Apply the supersede in the same rewrite below.
                std::string sup = trim(opts.supersedes_id);
                std::size_t sup_dropped = 0;
                if (!sup.empty()) {
                    auto before = existing.size();
                    existing.erase(
                        std::remove_if(existing.begin(), existing.end(),
                            [&](const Record& r){ return r.id == sup; }),
                        existing.end());
                    sup_dropped = before - existing.size();
                }
                std::string err = write_records(p, existing);
                if (!err.empty()) {
                    res.error = "remember: " + err;
                    return res;
                }
                res.id = rec.id;
                res.deduped = true;
                std::string add = "deduped: refreshed existing record (hits="
                                + std::to_string(rec.hits) + ")";
                if (!sup.empty()) {
                    add += sup_dropped
                        ? "; superseded " + sup
                        : "; supersede id " + sup + " not found";
                }
                if (res.note.empty()) res.note = std::move(add);
                else { res.note += "; "; res.note += add; }
                bump_cache(actual_scope);
                return res;
            }
        }
    }

    // ── Supersede pass ───────────────────────────────────────────────
    // Drop the named predecessor (in either scope) inside the same
    // rewrite that adds the new record. A missing id is non-fatal —
    // we just note it. This is the atomic edit-an-existing-fact path.
    std::string sup = trim(opts.supersedes_id);
    bool sup_hit = false;
    if (!sup.empty()) {
        auto before = existing.size();
        existing.erase(
            std::remove_if(existing.begin(), existing.end(),
                [&](const Record& r){ return r.id == sup; }),
            existing.end());
        sup_hit = (existing.size() != before);
        if (!sup_hit) {
            // The id might live in the OTHER scope. Drop it there too
            // — keeps the supersede semantics consistent regardless of
            // which scope the model thought it was editing.
            Scope other = (actual_scope == Scope::User) ? Scope::Project : Scope::User;
            auto op = path_for(other);
            if (!op.empty()) {
                auto other_recs = read_records(op);
                auto ob = other_recs.size();
                other_recs.erase(
                    std::remove_if(other_recs.begin(), other_recs.end(),
                        [&](const Record& r){ return r.id == sup; }),
                    other_recs.end());
                if (other_recs.size() != ob) {
                    (void)write_records(op, other_recs);
                    bump_cache(other);
                    sup_hit = true;
                }
            }
        }
        std::string add = sup_hit
            ? "superseded " + sup
            : "supersede id " + sup + " not found (new record still written)";
        if (res.note.empty()) res.note = std::move(add);
        else { res.note += "; "; res.note += add; }
    }

    // ── Build the new record ─────────────────────────────────────────
    Record r;
    r.id     = make_id();
    r.ts     = now_unix();
    r.scope  = actual_scope;
    r.text   = std::move(body);
    r.pinned = opts.pinned;
    r.tags   = std::move(tags_norm);
    r.hits   = 0;

    // ── Cap rollover ─────────────────────────────────────────────────
    // Pinned records are cap-exempt: walk oldest-first and drop the
    // first UNPINNED record until we fit. Falling back to dropping
    // pinned only if everything in the file is pinned and we still
    // overflow — in that degenerate case the user has more pinned
    // facts than the cap can hold, and the oldest pinned has to go.
    auto drop_oldest_unpinned = [](std::vector<Record>& v) -> bool {
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (!it->pinned) { v.erase(it); return true; }
        }
        return false;
    };
    bool rollover = false;
    while (existing.size() + 1 > kMaxRecordsPerScope) {
        bool dropped = drop_oldest_unpinned(existing);
        if (!dropped) {
            // All-pinned overflow — sacrifice the oldest pinned.
            existing.erase(existing.begin());
        }
        ++res.rolled;
        rollover = true;
    }
    existing.push_back(r);
    auto total_bytes = [](const std::vector<Record>& rs) {
        std::size_t b = 0;
        for (const auto& x : rs) b += serialise_record(x).size() + 1;
        return b;
    };
    while (existing.size() > 1 && total_bytes(existing) > kMaxFileBytes) {
        if (!drop_oldest_unpinned(existing)) {
            // All-pinned overflow on byte cap — drop the oldest pinned
            // that isn't the record we just pushed.
            if (existing.size() > 1) existing.erase(existing.begin());
            else break;
        }
        ++res.rolled;
        rollover = true;
    }

    std::string err;
    if (rollover || sup_hit) {
        // Any rewrite-shaped mutation goes through the atomic path.
        err = write_records(p, existing);
    } else {
        err = append_line(p, serialise_record(r));
    }
    if (!err.empty()) {
        res.error = "remember: " + err;
        return res;
    }
    res.id = r.id;
    bump_cache(actual_scope);
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

// UTF-8-safe clip to at most `cap` bytes, ending on a complete code point.
static std::string clip_text(const std::string& s, std::size_t cap) {
    if (s.size() <= cap) return s;
    std::size_t cut = cap;
    for (int back = 0; back < 4 && cut > 0; ++back, --cut) {
        unsigned char c = static_cast<unsigned char>(s[cut]);
        if ((c & 0xC0) != 0x80) break;   // leading byte → safe boundary
    }
    std::string out = s.substr(0, cut);
    out += "\xe2\x80\xa6";   // … — signals the prompt copy is clipped
    return out;
}

PromptSelection select_for_prompt(std::vector<Record> recent,
                                  std::size_t byte_budget) {
    PromptSelection sel;
    if (recent.empty()) return sel;

    // Approximate per-record prompt cost: render_for_prompt adds an
    // `[id] ` prefix (~11 bytes), an optional ★, a tag suffix, and a
    // trailing newline. Charge the clipped text length + a fixed chrome
    // estimate so the budget reflects what actually lands in the prompt.
    auto cost_of = [](const Record& r) -> std::size_t {
        std::size_t c = std::min(r.text.size(), kPromptRecordCap);
        c += 16;                                   // [id] + ★ + newline
        for (const auto& tg : r.tags) c += tg.size() + 2;
        return c;
    };

    // Rank for budget-fill: pinned first, then more hits, then more
    // recent. We keep an index list so the FINAL emission can be restored
    // to stable oldest-first order (recent[] is already oldest-first).
    std::vector<std::size_t> order(recent.size());
    for (std::size_t i = 0; i < recent.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&](std::size_t a, std::size_t b) {
            const Record& ra = recent[a];
            const Record& rb = recent[b];
            if (ra.pinned != rb.pinned) return ra.pinned;       // pinned wins
            if (ra.hits   != rb.hits)   return ra.hits > rb.hits;// more hits wins
            return ra.ts > rb.ts;                                // more recent wins
        });

    // Walk the ranked order, admitting records until the byte budget is
    // spent. Pinned records are admitted even past the budget (they are
    // load-bearing by definition); everything else respects the cap.
    std::vector<char> keep(recent.size(), false);
    std::size_t spent = 0;
    for (std::size_t idx : order) {
        const std::size_t c = cost_of(recent[idx]);
        if (recent[idx].pinned || spent + c <= byte_budget) {
            keep[idx] = true;
            spent += c;
        }
    }

    // Emit kept records in stable oldest-first order, clipping any
    // over-long text to the per-record cap. Count the drops.
    for (std::size_t i = 0; i < recent.size(); ++i) {
        if (!keep[i]) { ++sel.dropped; continue; }
        Record r = std::move(recent[i]);
        r.text = clip_text(r.text, kPromptRecordCap);
        sel.records.push_back(std::move(r));
    }
    return sel;
}

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
    out.reserve(r.text.size() + 32);
    out += '[';
    out += r.id;
    out += "] ";
    if (r.pinned) out += "\xe2\x98\x85 ";   // ★ — visual cue for pinned
    // A record can round-trip arbitrary bytes from a pasted tool output or a
    // clipboard capture (e.g. a truncated multi-byte UTF-8 sequence). Repair
    // it here so the system prompt — which is serialized by the provider's
    // JSON dump — is always well-formed. Cheap no-op when already valid.
    out += tools::util::to_valid_utf8(r.text);
    if (!r.tags.empty()) {
        out += "  {";
        for (std::size_t i = 0; i < r.tags.size(); ++i) {
            if (i) out += ", ";
            out += r.tags[i];
        }
        out += '}';
    }
    return out;
}

std::optional<std::pair<Record, Scope>> find_by_id(std::string_view id) {
    std::string want{id};
    if (trim(want).empty()) return std::nullopt;
    std::lock_guard lk(store_mu());
    for (auto s : {Scope::User, Scope::Project}) {
        const auto p = path_for(s);
        if (p.empty()) continue;
        auto recs = read_records(p);
        for (auto& r : recs)
            if (r.id == want) return std::make_pair(std::move(r), s);
    }
    return std::nullopt;
}

std::vector<Record> preview_forget_by_substring(std::string_view needle) {
    std::vector<Record> out;
    std::string want = trim(needle);
    if (want.empty()) return out;
    std::lock_guard lk(store_mu());
    for (auto s : {Scope::User, Scope::Project}) {
        const auto p = path_for(s);
        if (p.empty()) continue;
        for (auto& r : read_records(p))
            if (r.text.find(want) != std::string::npos)
                out.push_back(std::move(r));
    }
    return out;
}

std::optional<std::size_t> wipe(Scope s) {
    const auto p = path_for(s);
    if (p.empty()) return std::nullopt;
    std::lock_guard lk(store_mu());
    auto recs = read_records(p);
    std::size_t n = recs.size();
    // Truncate (write empty) rather than fs::remove — keeps the path
    // valid for subsequent append calls without re-running
    // create_directories. write_records on an empty vector writes a
    // zero-byte file, which read_records / load_all both handle.
    (void)write_records(p, {});
    bump_cache(s);
    return n;
}

} // namespace agentty::tools::memory
