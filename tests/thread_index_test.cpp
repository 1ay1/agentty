// thread_index_test.cpp — the mtime cache actually caches.
//
// The bug this pins: file_mtime_secs converted fs::file_time_type by
// reading time_since_epoch() directly. That clock's epoch is
// implementation-defined — on libstdc++ it sits around 1822 — so:
//
//   as nanoseconds  the value overflowed int64 and came out NEGATIVE
//   as seconds      no overflow, but still measured from 1822
//
// Either way the number in index.json was not a Unix timestamp. The second
// form is self-consistent, so the cache appeared to work; the first meant
// no entry ever matched its file and every startup re-parsed the entire
// history. With 605 threads and 704 MB on disk that is several seconds
// before the thread list exists, which shows up as "Alt+→ says there are no
// threads, then works on the second press".
//
// Both are invisible to a test that only checks load_all_threads returns
// the right threads — it does, either way, just slowly. So this checks the
// TIMESTAMP, which is the thing that was wrong.
#include "agentty/io/persistence.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++failures;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/// A scratch AGENTTY_HOME, so the test never touches the real one.
struct Sandbox {
    fs::path dir;
    Sandbox() {
        dir = fs::temp_directory_path() /
              ("agentty-idx-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        ::setenv("AGENTTY_HOME", dir.c_str(), 1);
    }
    ~Sandbox() { fs::remove_all(dir); }
};

Thread make_thread(const std::string& id, const std::string& title) {
    Thread t;
    t.id = ThreadId{id};
    t.title = title;
    t.created_at = std::chrono::system_clock::now();
    t.updated_at = t.created_at;
    return t;
}

/// The index's own record of when each file was written.
long long indexed_mtime(const std::string& id) {
    const auto p = persistence::threads_dir() / "index.json";
    std::ifstream in(p);
    if (!in) return -1;
    std::string all((std::istreambuf_iterator<char>(in)), {});
    // Deliberately not parsed with nlohmann: this test is about what is IN
    // the file, and reading it the same way the code wrote it would hide a
    // serialisation bug. Find "<id>" then the next "mtime": after it.
    const auto at = all.find('"' + id + '"');
    if (at == std::string::npos) return -1;
    const auto key = all.find("\"mtime\"", at);
    if (key == std::string::npos) return -1;
    const auto colon = all.find(':', key);
    return std::strtoll(all.c_str() + colon + 1, nullptr, 10);
}

void the_index_records_a_unix_timestamp() {
    std::printf("the index records a real timestamp\n");
    Sandbox sb;

    persistence::save_thread(make_thread("aaaa000000000001", "one"));
    persistence::flush_pending_saves();
    (void)persistence::load_all_threads();   // builds the index

    const long long mt = indexed_mtime("aaaa000000000001");
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    // Positive, obviously — the negative value was the original bug.
    CHECK(mt > 0);
    // And actually NOW, not 1822 and not 2286: within a minute of the
    // clock the rest of the program uses. This is the check the "seconds
    // since file_clock's epoch" version fails, and it is the one that makes
    // the number mean something to anything else reading the file.
    CHECK(mt > now - 60 && mt < now + 60);

    std::printf("  mtime %lld, now %lld (%+lld s)\n", mt, now, mt - now);
}

void a_warm_index_is_reused() {
    std::printf("an unchanged thread is not re-parsed\n");
    Sandbox sb;

    persistence::save_thread(make_thread("aaaa000000000002", "two"));
    persistence::flush_pending_saves();

    const auto first = persistence::load_all_threads();
    CHECK(first.size() == 1);
    const long long mt1 = indexed_mtime("aaaa000000000002");

    // Second walk over an untouched file. The entry has to survive
    // verbatim: if the stamp does not match, the file is re-parsed and the
    // index rewritten, which is exactly the slow path that was always
    // taken.
    const auto second = persistence::load_all_threads();
    CHECK(second.size() == 1);
    CHECK(indexed_mtime("aaaa000000000002") == mt1);
    if (!second.empty()) CHECK(second[0].title == "two");

    std::printf("  stamp unchanged across two loads (%lld)\n", mt1);
}

void a_changed_thread_is_re_read() {
    std::printf("a modified thread IS re-parsed\n");
    Sandbox sb;

    persistence::save_thread(make_thread("aaaa000000000003", "before"));
    persistence::flush_pending_saves();
    (void)persistence::load_all_threads();

    // Rewrite it with a different title. The cache must NOT serve the old
    // one — a stamp that never changes is as wrong as one that never
    // matches, just in the other direction.
    auto t = make_thread("aaaa000000000003", "after");
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    const auto again = persistence::load_all_threads();
    CHECK(again.size() == 1);
    if (!again.empty()) CHECK(again[0].title == "after");

    std::printf("  the new title came back, not the cached one\n");
}

void an_index_from_an_older_version_is_discarded() {
    std::printf("an index written by an older version is dropped\n");
    Sandbox sb;

    persistence::save_thread(make_thread("aaaa000000000004", "four"));
    persistence::flush_pending_saves();
    (void)persistence::load_all_threads();

    // Forge a v3 index: the shape is right, but its stamps are measured
    // from file_clock's epoch. Reading it would resurrect the bug, so the
    // version gate has to throw it away.
    const auto p = persistence::threads_dir() / "index.json";
    {
        std::ofstream out(p, std::ios::trunc);
        out << R"({"version":3,"threads":{"aaaa000000000004":)"
            << R"({"created_at":0,"mtime":-4650357762,"size":1,"title":"stale"}}})";
    }

    const auto loaded = persistence::load_all_threads();
    CHECK(loaded.size() == 1);
    // The title comes from the FILE, not from the forged entry.
    if (!loaded.empty()) CHECK(loaded[0].title == "four");
    // And the rebuilt index carries a sane stamp again.
    CHECK(indexed_mtime("aaaa000000000004") > 0);

    std::printf("  v3 discarded; rebuilt from the files\n");
}

}  // namespace

int main() {
    std::printf("agentty thread index\n\n");
    the_index_records_a_unix_timestamp();
    a_warm_index_is_reused();
    a_changed_thread_is_re_read();
    an_index_from_an_older_version_is_discarded();
    std::printf("\n%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
