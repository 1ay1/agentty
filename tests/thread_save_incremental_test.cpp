// thread_save_incremental_test — a save writes only what changed.
//
// save_thread() used to rewrite and re-verify the whole log every round,
// which made a long thread slower the longer it ran. It now cuts the log at
// the first changed message and appends from there. These tests pin both
// halves: the tail path is really taken (history before the change is not
// touched), and every kind of change still lands correctly on disk.

#include "agtest.hpp"

#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

Message make_msg(std::size_t i) {
    Message m;
    m.role = (i % 2 == 0) ? Role::User : Role::Assistant;
    m.id   = MessageId{"m" + std::to_string(i)};
    m.text = "message " + std::to_string(i);
    return m;
}

Thread make_thread(std::string id, std::size_t n) {
    Thread t;
    t.id    = ThreadId{std::move(id)};
    t.title = "incremental subject";
    for (std::size_t i = 0; i < n; ++i) t.messages.push_back(make_msg(i));
    return t;
}

fs::path log_path(const Thread& t) {
    return persistence::threads_dir() / (t.id.value + ".jsonl");
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

void save(const Thread& t) {
    persistence::save_thread(t);
    persistence::flush_pending_saves();
}

Thread load(const Thread& t) {
    auto got = persistence::load_thread_by_id(t.id);
    REQUIRE(got.has_value());
    return std::move(*got);
}

void check_same(const Thread& want, const Thread& got) {
    REQUIRE(got.messages.size() == want.messages.size());
    for (std::size_t i = 0; i < want.messages.size(); ++i) {
        CHECK(got.messages[i].id.value == want.messages[i].id.value);
        CHECK(got.messages[i].text == want.messages[i].text);
    }
}

// Mark message 0 on disk without changing any length, so offsets stay
// valid. A save that leaves the mark in place did not rewrite the head.
void mark_head(const Thread& t) {
    auto s = slurp(log_path(t));
    const auto at = s.find("message 0\"");
    REQUIRE(at != std::string::npos);
    s.replace(at, 9, "MESSAGE 0");
    spit(log_path(t), s);
}
bool head_marked(const Thread& t) {
    return slurp(log_path(t)).find("MESSAGE 0") != std::string::npos;
}

} // namespace

TEST_CASE("incremental save: a new message is appended, history untouched") {
    Thread t = make_thread("inc-append", 6);
    persistence::delete_thread(t.id);
    save(t);                         // first save of the process: full
    mark_head(t);

    t.messages.push_back(make_msg(6));
    save(t);                         // tail save: only message 6

    CHECK_MESSAGE(head_marked(t), "the head was rewritten; the tail path was not taken");
    auto got = load(t);
    REQUIRE(got.messages.size() == 7u);
    CHECK(got.messages[6].text == "message 6");
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: the open turn is replaced, not duplicated") {
    Thread t = make_thread("inc-open-turn", 4);
    persistence::delete_thread(t.id);
    save(t);
    mark_head(t);

    // The last message keeps changing while it streams / its tools settle.
    for (int round = 0; round < 5; ++round) {
        t.messages.back().text = "streaming " + std::to_string(round);
        save(t);
    }
    CHECK(head_marked(t));
    auto got = load(t);
    REQUIRE(got.messages.size() == 4u);
    CHECK(got.messages.back().text == "streaming 4");
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: editing an old message rewrites from there") {
    Thread t = make_thread("inc-edit", 8);
    persistence::delete_thread(t.id);
    save(t);

    // Same length on purpose: a size-only fingerprint would miss this.
    t.messages[3].text = "MESSAGE 3";
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: a shorter thread (rewind) truncates the log") {
    Thread t = make_thread("inc-shrink", 10);
    persistence::delete_thread(t.id);
    save(t);

    t.messages.resize(4);
    t.messages.push_back(make_msg(99));
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: a replaced prefix (compaction/fork) still lands") {
    Thread t = make_thread("inc-replace", 6);
    persistence::delete_thread(t.id);
    save(t);

    Thread fresh = make_thread("inc-replace", 3);
    fresh.messages[0].text = "a completely different head";
    save(fresh);
    check_same(fresh, load(fresh));
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: Smart Mode cards don't shift the log indices") {
    Thread t = make_thread("inc-smart-card", 4);
    persistence::delete_thread(t.id);
    save(t);

    Message card;
    card.smart_routing = true;          // view-only, never persisted
    t.messages.insert(t.messages.begin() + 2, card);
    t.messages.push_back(make_msg(4));
    save(t);

    auto got = load(t);
    REQUIRE(got.messages.size() == 5u);
    CHECK(got.messages[2].id.value == "m2");
    CHECK(got.messages[4].id.value == "m4");
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: after delete, the next save is a full write") {
    Thread t = make_thread("inc-delete", 5);
    persistence::delete_thread(t.id);
    save(t);
    persistence::delete_thread(t.id);
    CHECK_FALSE(fs::exists(log_path(t)));

    // The writer must not think the (now missing) log still holds 5 lines.
    t.messages.push_back(make_msg(5));
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(t.id);
}

TEST_CASE("incremental save: a log damaged behind the writer's back heals") {
    Thread t = make_thread("inc-damaged", 5);
    persistence::delete_thread(t.id);
    save(t);

    // Someone truncates the log to nothing. The tail save sees the log is
    // shorter than it thinks; it can't rebuild the head from disk, so it
    // drops its bookkeeping and the NEXT save goes full and heals it.
    spit(log_path(t), "");
    fs::remove(persistence::threads_dir() / (t.id.value + ".ofs"));
    t.messages.push_back(make_msg(5));
    save(t);
    t.messages.push_back(make_msg(6));
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(t.id);
}

TEST_CASE("thread log: truncate_to cuts both files and appends cleanly") {
    const ThreadId id{"inc-truncate-unit"};
    if (auto l = ThreadLog::open(id)) l->remove();
    auto log = ThreadLog::open(id);
    REQUIRE(log.has_value());
    for (std::size_t i = 0; i < 6; ++i) REQUIRE(log->append(make_msg(i)));

    REQUIRE(log->truncate_to(3));
    CHECK(log->size() == 3u);
    REQUIRE(log->append(make_msg(42)));

    auto again = ThreadLog::open(id);   // re-read the FILES, not memory
    REQUIRE(again.has_value());
    const auto all = again->all();
    REQUIRE(all.size() == 4u);
    CHECK(all[2].id.value == "m2");
    CHECK(all[3].id.value == "m42");
    CHECK(log->truncate_to(100));       // past the end: no-op, no error
    CHECK(log->size() == 4u);
    again->remove();
}
