// thread_log_test — the append-only message log and its offset index.
//
// The master property is ROUND-TRIP IDENTITY: a thread written to a log
// and read back must equal the thread that went in. Everything else here
// exists to make the failure modes explicit rather than discovered:
//
//   * the index is a CACHE — deleting, truncating or corrupting it must
//     change nothing except how long the open takes,
//   * a torn append costs the partial line and nothing else,
//   * append is O(1) in thread size, which is the property that silently
//     regresses if someone later "simplifies" the writer,
//   * a windowed read returns exactly the same messages as a full read.
//
// These are written against the real codec (persistence::message_to_json /
// message_from_json), so they also pin that the log and the whole-document
// format cannot drift apart.

#include "agtest.hpp"

#include <agentty/io/thread_log.hpp>
#include <agentty/io/persistence.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

fs::path scratch_dir() {
    auto p = fs::temp_directory_path() / "agentty_thread_log_test";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// A fresh log path per test, so cases can't contaminate each other.
fs::path fresh_log(std::string_view name) {
    auto p = scratch_dir() / (std::string{name} + ".jsonl");
    std::error_code ec;
    fs::remove(p, ec);
    auto idx = p; idx.replace_extension(".ofs");
    fs::remove(idx, ec);
    return p;
}

// Bytes that are hostile to a text pipeline, since tool output really does
// carry them: a UTF-8 sequence, characters that must be JSON-escaped, and
// — critically — an embedded NEWLINE, which is the one thing a
// line-delimited format could plausibly get wrong.
Message make_message(int i, bool nasty = false) {
    Message m;
    m.role = (i % 2 == 0) ? Role::User : Role::Assistant;
    m.id   = MessageId{"m" + std::to_string(i)};
    m.text = "message " + std::to_string(i);
    if (nasty) {
        m.text += "\nsecond line\twith tab \"quoted\" \\ backslash "
                  "\xE2\x80\x94 em dash \xF0\x9F\x9A\x80 rocket";
    }
    return m;
}

std::vector<Message> make_messages(std::size_t n, bool nasty = false) {
    std::vector<Message> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        v.push_back(make_message(static_cast<int>(i), nasty));
    return v;
}

// Compare on the fields the log is responsible for carrying.
bool same(const Message& a, const Message& b) {
    return a.id.value == b.id.value
        && a.role      == b.role
        && a.text      == b.text
        && a.thinking  == b.thinking
        && a.tool_calls.size() == b.tool_calls.size()
        && a.images.size()     == b.images.size();
}

bool same_all(const std::vector<Message>& a, const std::vector<Message>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("thread log: round-trips messages verbatim") {
    const auto path = fresh_log("roundtrip");
    const auto sent = make_messages(50, /*nasty=*/true);

    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->empty());
        for (const auto& m : sent) CHECK(log->append(m));
        CHECK(log->size() == sent.size());
    }

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->size() == sent.size());
    CHECK_MESSAGE(same_all(reopened->all(), sent),
                  "every message must survive a write/read cycle verbatim, "
                  "embedded newlines and all");
}

TEST_CASE("thread log: an embedded newline does not split a message") {
    // The obvious way to break a line-delimited format. message_to_json
    // escapes it, but that guarantee deserves a test rather than trust.
    const auto path = fresh_log("newlines");
    Message m;
    m.role = Role::Assistant;
    m.id   = MessageId{"nl"};
    m.text = "line one\nline two\nline three";

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->append(m));

    CHECK_MESSAGE(log->size() == 1u,
                  "a message containing newlines is still ONE log entry");
    const auto back = log->all();
    REQUIRE(back.size() == 1u);
    CHECK(back[0].text == m.text);

    // And the file really does hold one line.
    const std::string raw = read_file(path);
    CHECK(std::count(raw.begin(), raw.end(), '\n') == 1);
}

TEST_CASE("thread log: windowed reads match the full read") {
    const auto path = fresh_log("window");
    const auto sent = make_messages(200);
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(sent));
    }

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const auto all = log->all();
    REQUIRE(all.size() == sent.size());

    // The thread-switch shape: the last screenful.
    const auto tail = log->range(log->size() - 60, log->size());
    REQUIRE(tail.size() == 60u);
    for (std::size_t i = 0; i < tail.size(); ++i)
        CHECK(same(tail[i], all[all.size() - 60 + i]));

    // A window from the middle, and the degenerate cases.
    const auto mid = log->range(40, 60);
    REQUIRE(mid.size() == 20u);
    CHECK(same(mid.front(), all[40]));
    CHECK(same(mid.back(),  all[59]));

    CHECK(log->range(10, 10).empty());          // empty window
    CHECK(log->range(50, 10).empty());          // reversed
    CHECK(log->range(9999, 10000).empty());     // past the end
    CHECK(log->range(190, 9999).size() == 10u); // clamped to the log
}

TEST_CASE("thread log: the index is a cache, not truth") {
    const auto path = fresh_log("cache");
    auto idx = path; idx.replace_extension(".ofs");
    const auto sent = make_messages(120, /*nasty=*/true);
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(sent));
    }
    CHECK(fs::exists(idx));

    // 8 bytes per message, and nothing else.
    CHECK(fs::file_size(idx) == sent.size() * 8u);

    auto expect_intact = [&](const char* what) {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK_MESSAGE(log->size() == sent.size(), what);
        CHECK_MESSAGE(same_all(log->all(), sent), what);
    };

    // 1. Deleted outright.
    std::error_code ec;
    fs::remove(idx, ec);
    expect_intact("a deleted index must be rebuilt, losing nothing");
    CHECK_MESSAGE(fs::exists(idx), "and rewritten for next time");

    // 2. Truncated (the crash-between-appends case).
    {
        std::ofstream out(idx, std::ios::binary | std::ios::trunc);
        out << std::string(40, '\0');    // 5 offsets, all zero
    }
    expect_intact("a truncated index must be rebuilt");

    // 3. Filled with garbage that is not even offset-shaped.
    {
        std::ofstream out(idx, std::ios::binary | std::ios::trunc);
        out << "this is not an index at all";
    }
    expect_intact("a corrupt index must be rebuilt");

    // 4. Offsets that are well-formed but point outside the log.
    {
        std::string buf(sent.size() * 8u, '\0');
        for (std::size_t i = 0; i < sent.size(); ++i) {
            const std::uint64_t bogus = 1ull << 40;
            for (int b = 0; b < 8; ++b)
                buf[i * 8 + static_cast<std::size_t>(b)] =
                    static_cast<char>((bogus >> (b * 8)) & 0xFF);
        }
        std::ofstream out(idx, std::ios::binary | std::ios::trunc);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    }
    expect_intact("an out-of-range index must be rebuilt");
}

TEST_CASE("thread log: a torn append costs only the partial line") {
    const auto path = fresh_log("torn");
    const auto sent = make_messages(80);
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(sent));
    }

    // Simulate a crash mid-append: chop the file inside its last line,
    // and drop the index so the reader has to cope on its own.
    std::string raw = read_file(path);
    const std::size_t last_nl = raw.find_last_of('\n', raw.size() - 2);
    REQUIRE(last_nl != std::string::npos);
    raw.resize(last_nl + 1 + 20);           // 20 bytes into the final line
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    }
    std::error_code ec;
    auto idx = path; idx.replace_extension(".ofs");
    fs::remove(idx, ec);

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const auto back = log->all();
    CHECK_MESSAGE(back.size() == sent.size() - 1,
                  "every complete message survives; only the torn one is lost");
    for (std::size_t i = 0; i < back.size(); ++i) CHECK(same(back[i], sent[i]));

    // And the log is still usable: appending after the tear works.
    CHECK(log->append(make_message(9999)));
    auto after = ThreadLog::open_path(path);
    REQUIRE(after.has_value());
    CHECK(after->all().size() == sent.size());
}

TEST_CASE("thread log: append is O(1) in thread size") {
    // The property that silently regresses if the writer is ever
    // "simplified" back into rewriting the file. Measured in BYTES
    // written, not time, so it is deterministic on any machine.
    const auto path = fresh_log("append_cost");
    const auto sent = make_messages(500);
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(sent));
    }

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const auto before = fs::file_size(path);

    Message extra = make_message(1000);
    CHECK(log->append(extra));

    const auto after = fs::file_size(path);
    const auto grew  = after - before;
    CHECK_MESSAGE(grew < 1024u,
                  "appending one message must add roughly one line, "
                  "not rewrite the log");
    CHECK(log->size() == sent.size() + 1);

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->size() == sent.size() + 1);
    CHECK(same(reopened->all().back(), extra));
}

TEST_CASE("thread log: rewrite replaces history atomically") {
    const auto path = fresh_log("rewrite");
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(make_messages(100)));
        CHECK(log->size() == 100u);
    }
    // Compaction / edit / fork shape: fewer messages than before.
    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const auto shorter = make_messages(10);
    CHECK(log->rewrite(shorter));
    CHECK(log->size() == 10u);

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    CHECK_MESSAGE(reopened->size() == 10u,
                  "the index must shrink with the log, not keep stale offsets");
    CHECK(same_all(reopened->all(), shorter));
}

TEST_CASE("thread log: an empty or absent log is a valid empty thread") {
    const auto path = fresh_log("empty");
    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->empty());
    CHECK(log->size() == 0u);
    CHECK(log->all().empty());
    CHECK(log->range(0, 100).empty());

    // A zero-byte file left by an interrupted create is the same thing.
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); }
    auto again = ThreadLog::open_path(path);
    REQUIRE(again.has_value());
    CHECK(again->empty());
    CHECK(again->append(make_message(0)));
    CHECK(again->size() == 1u);
}
