// thread_log_seam_probe — does the STORE actually prefer the log?
//
// thread_log_test proves the log round-trips. This proves the seam: that
// persistence::load_thread_by_id returns the log when one exists, the
// legacy document when it doesn't, and the SAME thread either way.
//
// Run against a real thread, in an isolated AGENTTY_HOME:
//
//   AGENTTY_HOME=/tmp/x ./build/agentty_standalone_tests \
//       thread_log_seam_probe <id>
//
// It converts the legacy <id>.json into the log format, reads back
// through the store seam, and compares field by field — including
// materialised image bytes, which is where a broken blob reference would
// hide. No arguments = no-op pass, so ctest costs nothing.

#include <agentty/domain/conversation.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

long long ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}

bool same_thread(const Thread& a, const Thread& b, std::string& why) {
    auto fail = [&](const char* f) { why = f; return false; };
    if (a.id.value     != b.id.value)     return fail("id");
    if (a.title        != b.title)        return fail("title");
    if (a.forked_from  != b.forked_from)  return fail("forked_from");
    if (a.messages.size() != b.messages.size()) return fail("message count");
    if (a.compactions.size() != b.compactions.size())
        return fail("compaction count");
    for (std::size_t i = 0; i < a.messages.size(); ++i) {
        const auto& x = a.messages[i];
        const auto& y = b.messages[i];
        if (x.id.value != y.id.value) return fail("message id");
        if (x.role     != y.role)     return fail("message role");
        if (x.text     != y.text)     return fail("message text");
        if (x.thinking != y.thinking) return fail("message thinking");
        if (x.tool_calls.size() != y.tool_calls.size())
            return fail("tool_calls size");
        for (std::size_t k = 0; k < x.tool_calls.size(); ++k)
            if (x.tool_calls[k].output() != y.tool_calls[k].output())
                return fail("tool output");
        if (x.images.size() != y.images.size()) return fail("images size");
        for (std::size_t k = 0; k < x.images.size(); ++k)
            if (x.images[k].bytes() != y.images[k].bytes())
                return fail("image bytes");
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("thread_log_seam_probe: no thread given, nothing to do.\n"
                    "usage: %s <path-to-thread.json>\n", argv[0]);
        return 0;
    }

    // The standalone harness points AGENTTY_HOME at a fresh sandbox so a
    // test can never touch real history — which is right, and means the
    // probe must COPY the thread it was given into that sandbox rather
    // than read it in place. Everything below then runs entirely inside
    // the sandbox, including the delete at the end.
    const fs::path src = argv[1];
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        std::printf("FAIL: %s is not a file\n", src.string().c_str());
        return 1;
    }
    const ThreadId id{src.stem().string()};
    const auto dst = persistence::threads_dir() / (id.value + ".json");
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::printf("FAIL: could not stage %s into the sandbox: %s\n",
                    src.string().c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("staged %s (%.1f MB) into %s\n", id.value.c_str(),
                static_cast<double>(fs::file_size(dst, ec)) / 1e6,
                persistence::threads_dir().string().c_str());

    // 1. Read via the seam BEFORE any log exists — must take the legacy path.
    auto t0 = std::chrono::steady_clock::now();
    auto legacy = persistence::load_thread_by_id(id);
    const auto legacy_ms = ms_since(t0);
    if (!legacy) {
        std::printf("FAIL: could not load %s at all\n", id.value.c_str());
        return 1;
    }
    std::printf("legacy load: %lld ms, %zu messages\n",
                legacy_ms, legacy->messages.size());

    const auto log_path = persistence::threads_dir() / (id.value + ".jsonl");
    check(!fs::exists(log_path), "no log should exist yet");

    // 2. Convert to the log format.
    {
        auto log = ThreadLog::open(id);
        if (!log) { std::printf("FAIL: could not open log\n"); return 1; }
        check(log->store_thread(*legacy), "store_thread must succeed");
    }
    check(fs::exists(log_path), "the log must now exist");

    // 3. Read via the seam AGAIN — must now take the log path, and must
    //    produce the same thread. This is the whole commit in one check.
    t0 = std::chrono::steady_clock::now();
    auto viaLog = persistence::load_thread_by_id(id);
    const auto log_ms = ms_since(t0);
    if (!viaLog) { std::printf("FAIL: log load returned nothing\n"); return 1; }
    std::printf("log load:    %lld ms, %zu messages\n",
                log_ms, viaLog->messages.size());

    std::string why;
    check(same_thread(*legacy, *viaLog, why),
          why.empty() ? "threads differ" : why.c_str());

    // 4. The picker walk must show exactly one row for this thread, not
    //    two (legacy + log) and not zero.
    const auto all = persistence::load_all_threads();
    std::size_t seen = 0;
    for (const auto& t : all) if (t.id.value == id.value) ++seen;
    check(seen == 1, "the thread list must show the thread exactly once");
    if (seen != 1)
        std::printf("  (saw it %zu times among %zu threads)\n", seen, all.size());

    // 5. Deleting must take the log with it.
    persistence::delete_thread(id);
    check(!fs::exists(log_path), "delete_thread must remove the log");
    check(!persistence::load_thread_by_id(id).has_value(),
          "a deleted thread must not load");

    if (failures == 0) {
        std::printf("PASS: the store prefers the log, and returns the same "
                    "thread either way\n");
        if (legacy_ms > 0)
            std::printf("      (%lld ms -> %lld ms)\n", legacy_ms, log_ms);
    }
    return failures == 0 ? 0 : 1;
}
