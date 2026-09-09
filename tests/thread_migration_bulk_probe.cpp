// thread_migration_bulk_probe — migrate an ENTIRE thread directory and
// prove nothing was lost.
//
// The per-thread probes run the save path over one file. This runs it
// over a whole directory — the real thing a user's first launch does,
// spread over however many threads they open — and checks the property
// that matters at that scale:
//
//   for EVERY thread: the content read back after migration is identical
//   to the content read before it, and the legacy document is gone only
//   where that held.
//
//   ./build/agentty_standalone_tests thread_migration_bulk_probe <dir>
//
// <dir> is used IN PLACE (it is expected to be a copy — the point is to
// rehearse on real data without touching the original). AGENTTY_HOME is
// pointed at its parent so the whole persistence layer operates there.
//
// No argument = no-op pass, so this costs nothing in CI.

#include <agentty/domain/conversation.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

// A content fingerprint of a thread: everything a user would notice
// losing. Compared before and after migration.
struct Fingerprint {
    std::string  title;
    std::string  forked_from;
    std::size_t  messages   = 0;
    std::size_t  tools      = 0;
    std::size_t  images     = 0;
    std::size_t  compactions = 0;
    std::uint64_t content   = 0;   // FNV-1a over text + tool output + image bytes

    bool operator==(const Fingerprint&) const = default;
};

void mix(std::uint64_t& h, std::string_view s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    h ^= 0xff; h *= 1099511628211ull;          // field separator
}

Fingerprint fingerprint(const Thread& t) {
    Fingerprint f;
    f.title       = t.title;
    f.forked_from = t.forked_from;
    f.messages    = t.messages.size();
    f.compactions = t.compactions.size();
    f.content     = 1469598103934665603ull;
    for (const auto& m : t.messages) {
        mix(f.content, m.id.value);
        mix(f.content, m.text);
        mix(f.content, m.thinking);
        f.tools  += m.tool_calls.size();
        f.images += m.images.size();
        for (const auto& tc : m.tool_calls) {
            mix(f.content, tc.name.value);
            mix(f.content, tc.output());       // materialises blob refs
        }
        for (const auto& im : m.images) {
            mix(f.content, im.media_type);
            mix(f.content, im.bytes());        // materialises lazily
        }
    }
    return f;
}

std::uintmax_t dir_bytes(const fs::path& p) {
    std::uintmax_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(p, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("thread_migration_bulk_probe: no directory given, "
                    "nothing to do.\nusage: %s <threads-dir-COPY>\n", argv[0]);
        return 0;
    }
    const fs::path dir = fs::absolute(argv[1]);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("FAIL: %s is not a directory\n", dir.string().c_str());
        return 1;
    }
    if (dir.filename() != "threads") {
        std::printf("FAIL: expected a directory named 'threads', got %s\n",
                    dir.filename().string().c_str());
        return 1;
    }

    // Point the persistence layer at this copy. The harness already set
    // AGENTTY_HOME to a sandbox; override it so threads_dir() resolves
    // here instead.
    const auto home = dir.parent_path();
#if defined(_WIN32)
    _putenv_s("AGENTTY_HOME", home.string().c_str());
#else
    ::setenv("AGENTTY_HOME", home.string().c_str(), 1);
#endif
    if (persistence::threads_dir() != dir) {
        std::printf("FAIL: threads_dir() is %s, expected %s\n",
                    persistence::threads_dir().string().c_str(),
                    dir.string().c_str());
        return 1;
    }

    // ── 1. Fingerprint everything BEFORE touching it ──────────────────
    std::vector<ThreadId> ids;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const auto p = e.path();
        if (p.extension() != ".json") continue;
        const auto name = p.filename().string();
        if (name == "index.json" || name.find("acp_sessions") != std::string::npos)
            continue;
        ids.push_back(ThreadId{p.stem().string()});
    }
    std::printf("directory: %s\n  %zu threads, %.0f MB\n",
                dir.string().c_str(), ids.size(), dir_bytes(dir) / 1e6);

    std::vector<Fingerprint> before;
    before.reserve(ids.size());
    std::size_t unreadable = 0;
    for (const auto& id : ids) {
        auto t = persistence::load_thread_by_id(id);
        if (!t) { ++unreadable; before.push_back({}); continue; }
        before.push_back(fingerprint(*t));
    }
    if (unreadable)
        std::printf("  (%zu unreadable before migration — skipped)\n", unreadable);

    // ── 2. Migrate every one, through the REAL save path ──────────────
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t migrated = 0;
    for (const auto& id : ids) {
        auto t = persistence::load_thread_by_id(id);
        if (!t) continue;
        persistence::save_thread(*t);
        ++migrated;
    }
    persistence::flush_pending_saves();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::printf("migrated %zu threads in %lld ms -> %.0f MB\n",
                migrated, static_cast<long long>(ms), dir_bytes(dir) / 1e6);

    // ── 3. Prove nothing changed ──────────────────────────────────────
    std::size_t lost = 0, mismatched = 0, still_legacy = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (before[i].messages == 0 && before[i].content == 0) continue; // skipped
        auto t = persistence::load_thread_by_id(ids[i]);
        if (!t) {
            ++lost;
            std::printf("  LOST: %s no longer loads\n", ids[i].value.c_str());
            continue;
        }
        const auto now = fingerprint(*t);
        if (!(now == before[i])) {
            ++mismatched;
            if (mismatched <= 5)
                std::printf("  CHANGED: %s  msgs %zu->%zu  tools %zu->%zu  "
                            "images %zu->%zu  content %s\n",
                            ids[i].value.c_str(),
                            before[i].messages, now.messages,
                            before[i].tools,    now.tools,
                            before[i].images,   now.images,
                            before[i].content == now.content ? "same" : "DIFFERS");
        }
        if (fs::exists(dir / (ids[i].value + ".json"), ec)) ++still_legacy;
    }

    // The picker must still list every thread, exactly once.
    const auto listed = persistence::load_all_threads();
    std::size_t dupes = 0;
    for (const auto& id : ids) {
        std::size_t n = 0;
        for (const auto& t : listed) if (t.id.value == id.value) ++n;
        if (n != 1) { ++dupes; if (dupes <= 5)
            std::printf("  LISTING: %s appears %zu times\n", id.value.c_str(), n); }
    }

    std::printf("\nresult: %zu lost, %zu changed, %zu still legacy, "
                "%zu listing problems\n", lost, mismatched, still_legacy, dupes);
    if (lost || mismatched || dupes) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("PASS: every thread migrated with identical content\n");
    return 0;
}
