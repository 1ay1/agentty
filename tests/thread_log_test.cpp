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
    auto idx = path; idx.replace_extension(".ofs");
    const auto sent = make_messages(500);
    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->rewrite(sent));
    }

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const auto before     = fs::file_size(path);
    const auto idx_before = fs::file_size(idx);

    Message extra = make_message(1000);
    CHECK(log->append(extra));

    const auto after = fs::file_size(path);
    const auto grew  = after - before;
    CHECK_MESSAGE(grew < 1024u,
                  "appending one message must add roughly one line, "
                  "not rewrite the log");
    // The INDEX has to be O(1) too. Rewriting it whole is only 8 bytes per
    // message — invisible at 500, but 400 KB per turn at 50k messages, and
    // unbounded after that. Exactly 8 bytes is the whole point.
    CHECK_MESSAGE(fs::file_size(idx) - idx_before == 8u,
                  "appending one message must add exactly one 8-byte offset");
    CHECK(log->size() == sent.size() + 1);

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    CHECK(reopened->size() == sent.size() + 1);
    CHECK(same(reopened->all().back(), extra));
}

TEST_CASE("thread log: many appends stay O(1) each") {
    // The shape a long conversation actually takes: one append per turn,
    // hundreds of times. Each must add exactly one line and exactly one
    // 8-byte offset, no matter how long the thread already is — if either
    // file is rewritten per turn the total work is quadratic in thread
    // length, which is invisible in a short test and fatal in a long one.
    // (The companion test below observes the rewrite directly.)
    const auto path = fresh_log("append_scaling");
    auto idx = path; idx.replace_extension(".ofs");
    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->rewrite(make_messages(200)));

    std::error_code ec;
    const auto before_size = fs::file_size(idx, ec);
    for (int i = 0; i < 50; ++i) CHECK(log->append(make_message(1000 + i)));
    CHECK_MESSAGE(fs::file_size(idx, ec) == before_size + 50u * 8u,
                  "50 appends must add exactly 50 offsets");

    for (int i = 0; i < 400; ++i) CHECK(log->append(make_message(2000 + i)));

    CHECK(fs::file_size(idx, ec) == log->size() * 8u);
    CHECK(log->size() == 650u);

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    CHECK_MESSAGE(reopened->size() == 650u,
                  "and every appended message must still be there");
    CHECK(same(reopened->all().back(), make_message(2399)));
}

TEST_CASE("thread log: appending does not rewrite the index") {
    // The O(1) guarantee, observed directly. write_json_atomic publishes
    // by RENAME, so a whole-index write replaces the file; an append
    // modifies it in place. fs::equivalent against a hard link taken
    // beforehand tells the two apart with no timing and no counters.
    const auto path = fresh_log("append_inplace");
    auto idx = path; idx.replace_extension(".ofs");
    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->rewrite(make_messages(300)));
    REQUIRE(fs::exists(idx));

    // A hard link pins the CURRENT file object. If append rewrites the
    // index, `idx` becomes a different object and equivalence breaks.
    const auto pin = idx;
    auto pinned = scratch_dir() / "append_inplace.pin";
    std::error_code ec;
    fs::remove(pinned, ec);
    fs::create_hard_link(pin, pinned, ec);
    REQUIRE_MESSAGE(!ec, "test needs hard links on the scratch filesystem");

    CHECK(log->append(make_message(9999)));

    const bool same_file = fs::equivalent(pin, pinned, ec) && !ec;
    CHECK_MESSAGE(same_file,
                  "append must write the index IN PLACE — a rewrite is "
                  "O(messages) per turn and unbounded in thread length");

    fs::remove(pinned, ec);
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

TEST_CASE("thread log: metadata round-trips beside the messages") {
    // A Thread is more than its messages. The header is mutable and the
    // log is append-only, so it lives in a sidecar — and every field of
    // it has to survive, or a reloaded thread quietly loses its title,
    // its fork provenance, or the compaction record that keeps the wire
    // payload small.
    const auto path = fresh_log("meta");
    auto meta = path; meta.replace_extension(".meta.json");
    std::error_code ec; fs::remove(meta, ec);

    Thread t;
    t.id          = ThreadId{"meta_thread"};
    t.title       = "a thread with a title";
    t.forked_from = "some_parent_id";
    t.rag_mode_override = store::RagMode::FirstTurnOnly;
    t.messages    = make_messages(30);
    Thread::CompactionRecord rec;
    rec.up_to_index = 10;
    rec.summary     = "summary of the first ten";
    t.compactions.push_back(rec);

    {
        auto log = ThreadLog::open_path(path);
        REQUIRE(log.has_value());
        CHECK(log->store_thread(t));
    }

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    const Thread back = log->load_thread();

    CHECK(back.id.value     == t.id.value);
    CHECK(back.title        == t.title);
    CHECK(back.forked_from  == t.forked_from);
    REQUIRE(back.rag_mode_override.has_value());
    CHECK(*back.rag_mode_override == store::RagMode::FirstTurnOnly);
    REQUIRE(back.compactions.size() == 1u);
    CHECK(back.compactions[0].up_to_index == 10u);
    CHECK(back.compactions[0].summary     == rec.summary);
    CHECK(same_all(back.messages, t.messages));

    // meta() alone must NOT drag the transcript in — that is the whole
    // point of splitting the files.
    CHECK_MESSAGE(log->meta().messages.empty(),
                  "metadata must not carry messages");
    CHECK(log->meta().title == t.title);
}

TEST_CASE("thread log: a compaction past the end of history is dropped") {
    // Only reachable via a save interrupted mid-compaction. The record
    // would make the wire summarise a prefix that no longer exists, so
    // the loader has to bound it — and in the log format the metadata is
    // read BEFORE the message count is known, which is exactly why the
    // clamp is a separate step rather than part of parsing.
    const auto path = fresh_log("stale_compaction");
    Thread t;
    t.id       = ThreadId{"stale"};
    t.messages = make_messages(5);
    Thread::CompactionRecord good; good.up_to_index = 3;
    Thread::CompactionRecord bad;  bad.up_to_index  = 999;
    t.compactions = {good, bad};

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->store_thread(t));

    auto reopened = ThreadLog::open_path(path);
    REQUIRE(reopened.has_value());
    const Thread back = reopened->load_thread();
    REQUIRE(back.compactions.size() == 1u);
    CHECK(back.compactions[0].up_to_index == 3u);
}

TEST_CASE("thread log: remove() clears every file of the thread") {
    // The log format is three files. Deleting a thread must take all of
    // them, or the thread reappears on the next directory walk.
    const auto path = fresh_log("removal");
    auto idx  = path; idx.replace_extension(".ofs");
    auto meta = path; meta.replace_extension(".meta.json");

    Thread t;
    t.id       = ThreadId{"removal"};
    t.title    = "doomed";
    t.messages = make_messages(5);

    auto log = ThreadLog::open_path(path);
    REQUIRE(log.has_value());
    CHECK(log->store_thread(t));
    REQUIRE(fs::exists(path));
    REQUIRE(fs::exists(idx));
    REQUIRE(fs::exists(meta));

    log->remove();
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(fs::exists(idx));
    CHECK_FALSE(fs::exists(meta));
    CHECK(log->empty());
    CHECK_FALSE(log->exists());
}
