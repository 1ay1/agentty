// thread_log_corpus_probe — round-trip every REAL thread through the log.
//
// The unit tests in thread_log_test.cpp use synthetic messages, which
// means they only exercise the shapes I thought to write. This probe runs
// the master property — write to a log, read it back, get the same thread
// — over the actual threads in ~/.agentty/threads, which contain shapes
// nobody would think to invent: tool calls that failed mid-stream, images
// from three different providers, thinking blocks, compaction markers,
// half-migrated blob references, and text in every encoding a shell can
// produce.
//
//   ./build/agentty_standalone_tests thread_log_corpus_probe [dir] [--verbose]
//
// Exit 0 = every thread round-tripped. Non-zero = the count that didn't,
// with the first few named.
//
// Not a ctest case with real data: it reads the user's home directory, so
// it no-ops (and passes) when the corpus isn't there. Run it by hand
// before trusting the migration with anyone's history.

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

struct Mismatch {
    std::string thread;
    std::string why;
};

// Compare a message before and after the log round-trip. Deliberately
// field-by-field rather than a serialise-and-diff: when this fails, the
// name of the field that changed is the whole debugging story.
bool compare(const Message& a, const Message& b, std::string& why) {
    auto fail = [&](const char* f) { why = f; return false; };
    if (a.id.value  != b.id.value)  return fail("id");
    if (a.role      != b.role)      return fail("role");
    if (a.text      != b.text)      return fail("text");
    if (a.thinking  != b.thinking)  return fail("thinking");
    if (a.thinking_signature != b.thinking_signature)
        return fail("thinking_signature");
    if (a.reasoning_summary  != b.reasoning_summary)
        return fail("reasoning_summary");
    if (a.images.size()     != b.images.size())     return fail("images.size");
    if (a.tool_calls.size() != b.tool_calls.size()) return fail("tool_calls.size");
    if (a.attachments.size() != b.attachments.size())
        return fail("attachments.size");

    for (std::size_t i = 0; i < a.images.size(); ++i) {
        if (a.images[i].media_type != b.images[i].media_type)
            return fail("image.media_type");
        // Materialise both: this is where a broken blob reference or a
        // lost lazy source would show up, and it is the failure that
        // would otherwise be invisible until a user re-sent the turn.
        if (a.images[i].bytes() != b.images[i].bytes())
            return fail("image.bytes");
    }
    for (std::size_t i = 0; i < a.tool_calls.size(); ++i) {
        const auto& x = a.tool_calls[i];
        const auto& y = b.tool_calls[i];
        if (x.id.value   != y.id.value)   return fail("tool.id");
        if (x.name.value != y.name.value) return fail("tool.name");
        if (x.output()   != y.output())   return fail("tool.output");
        if (x.status_name() != y.status_name()) return fail("tool.status");
    }
    for (std::size_t i = 0; i < a.attachments.size(); ++i) {
        if (a.attachments[i].kind != b.attachments[i].kind)
            return fail("attachment.kind");
        // Materialise both: this is where a lost blob reference or a
        // broken lazy source would show up, and it is exactly the failure
        // that stays invisible until the turn is re-sent.
        if (a.attachments[i].body.bytes() != b.attachments[i].body.bytes())
            return fail("attachment.body");
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    fs::path dir;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--verbose") verbose = true;
        else                  dir = a;
    }
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        if (!home) {
            std::printf("thread_log_corpus_probe: no HOME, nothing to do.\n");
            return 0;
        }
        dir = fs::path{home} / ".agentty" / "threads";
    }

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::printf("thread_log_corpus_probe: %s not present, nothing to do.\n",
                    dir.string().c_str());
        return 0;
    }

    // Somewhere to write the logs that is NOT the user's thread directory.
    const fs::path work = fs::temp_directory_path() / "agentty_log_corpus";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    std::size_t checked = 0, skipped = 0, msgs = 0;
    std::uintmax_t bytes = 0;
    std::vector<Mismatch> bad;
    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto p = e.path();
        if (p.extension() != ".json") continue;
        if (p.filename() == "index.json") continue;
        if (p.filename().string().find("acp_sessions") != std::string::npos) continue;

        auto loaded = persistence::load_thread_file(p);
        if (!loaded) { ++skipped; continue; }

        const std::string id = p.stem().string();
        auto log = ThreadLog::open_path(work / (id + ".jsonl"));
        if (!log) {
            bad.push_back({id, "could not open log"});
            continue;
        }
        if (!log->rewrite(loaded->messages)) {
            bad.push_back({id, "rewrite failed"});
            continue;
        }

        // Reopen from scratch so the index is read back off disk rather
        // than reused from memory — the read path a real switch takes.
        auto reopened = ThreadLog::open_path(work / (id + ".jsonl"));
        if (!reopened) { bad.push_back({id, "could not reopen"}); continue; }

        const auto back = reopened->all();
        if (back.size() != loaded->messages.size()) {
            bad.push_back({id, "message count " + std::to_string(back.size())
                               + " != " + std::to_string(loaded->messages.size())});
            continue;
        }

        std::string why;
        bool ok = true;
        for (std::size_t i = 0; i < back.size() && ok; ++i) {
            if (!compare(loaded->messages[i], back[i], why)) {
                bad.push_back({id, "messages[" + std::to_string(i) + "]." + why});
                ok = false;
            }
        }
        if (!ok) continue;

        // The windowed read must agree with the full read, since that is
        // the whole point of the offset index.
        if (back.size() > 60) {
            const auto tail = reopened->range(back.size() - 60, back.size());
            if (tail.size() != 60) {
                bad.push_back({id, "tail window size"});
                continue;
            }
            for (std::size_t i = 0; i < tail.size(); ++i) {
                if (!compare(back[back.size() - 60 + i], tail[i], why)) {
                    bad.push_back({id, "tail window differs: " + why});
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
        }

        ++checked;
        msgs  += back.size();
        bytes += fs::file_size(p, ec);
        if (verbose)
            std::printf("  ok %-20s %5zu msgs\n", id.c_str(), back.size());
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    std::printf("thread_log_corpus_probe: %zu threads, %zu messages, "
                "%.0f MB, %lld ms\n",
                checked, msgs, static_cast<double>(bytes) / 1e6,
                static_cast<long long>(ms));
    if (skipped) std::printf("  (%zu unreadable files skipped)\n", skipped);

    if (!bad.empty()) {
        std::printf("FAILED: %zu thread(s) did not round-trip\n", bad.size());
        for (std::size_t i = 0; i < bad.size() && i < 10; ++i)
            std::printf("  %-20s %s\n", bad[i].thread.c_str(),
                        bad[i].why.c_str());
        return 1;
    }
    std::printf("PASS: every thread round-tripped verbatim\n");
    fs::remove_all(work, ec);
    return 0;
}
