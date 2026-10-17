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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

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
    persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), t);
    persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());
}

Thread load(const Thread& t) {
    auto got = persistence::load_thread_by_id(::agentty::IoAccess::grant(), t.id);
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
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);                         // first save of the process: full
    mark_head(t);

    t.messages.push_back(make_msg(6));
    save(t);                         // tail save: only message 6

    CHECK_MESSAGE(head_marked(t), "the head was rewritten; the tail path was not taken");
    auto got = load(t);
    REQUIRE(got.messages.size() == 7u);
    CHECK(got.messages[6].text == "message 6");
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: the open turn is replaced, not duplicated") {
    Thread t = make_thread("inc-open-turn", 4);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
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
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: editing an old message rewrites from there") {
    Thread t = make_thread("inc-edit", 8);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);

    // Same length on purpose: a size-only fingerprint would miss this.
    t.messages[3].text = "MESSAGE 3";
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: a shorter thread (rewind) truncates the log") {
    Thread t = make_thread("inc-shrink", 10);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);

    t.messages.resize(4);
    t.messages.push_back(make_msg(99));
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: a replaced prefix (compaction/fork) still lands") {
    Thread t = make_thread("inc-replace", 6);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);

    Thread fresh = make_thread("inc-replace", 3);
    fresh.messages[0].text = "a completely different head";
    save(fresh);
    check_same(fresh, load(fresh));
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: Smart Mode cards don't shift the log indices") {
    Thread t = make_thread("inc-smart-card", 4);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
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
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: after delete, the next save is a full write") {
    Thread t = make_thread("inc-delete", 5);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    CHECK_FALSE(fs::exists(log_path(t)));

    // The writer must not think the (now missing) log still holds 5 lines.
    t.messages.push_back(make_msg(5));
    save(t);
    check_same(t, load(t));
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("incremental save: a log damaged behind the writer's back heals") {
    Thread t = make_thread("inc-damaged", 5);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
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
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

// The fingerprint is what decides which messages a save rewrites, so a
// change it FAILS to notice is silent data loss: the edit stays in memory,
// never reaches disk, and is gone on the next load. The hash mixes four
// independent lanes for speed, which is exactly the kind of change that can
// accidentally drop a byte range — so pin that every persisted field, and a
// difference at any offset of a long body, moves it.
TEST_CASE("incremental save: the fingerprint notices every persisted edit") {
    using persistence::debug_message_fingerprint;

    Message base;
    base.id   = MessageId{"fp-base"};
    base.role = Role::Assistant;
    base.text = "hello";
    const std::uint64_t h0 = debug_message_fingerprint(base);

    auto differs = [&](Message m, const char* what) {
        CHECK_MESSAGE(debug_message_fingerprint(m) != h0, what);
    };

    { Message m = base; m.id = MessageId{"fp-other"};      differs(std::move(m), "id"); }
    { Message m = base; m.role = Role::User;               differs(std::move(m), "role"); }
    { Message m = base; m.text = "hellp";                  differs(std::move(m), "text (one byte)"); }
    { Message m = base; m.text = "hello ";                 differs(std::move(m), "text (length)"); }
    { Message m = base; m.thinking = "t";                  differs(std::move(m), "thinking"); }
    { Message m = base; m.thinking_signature = "s";        differs(std::move(m), "thinking signature"); }
    { Message m = base; m.reasoning_encrypted = "e";       differs(std::move(m), "reasoning blob"); }
    { Message m = base; m.served_model = ModelId{"x"};     differs(std::move(m), "served model"); }
    { Message m = base; m.error = "boom";                  differs(std::move(m), "error"); }
    { Message m = base; m.is_compact_summary = true;       differs(std::move(m), "compact summary flag"); }
    { Message m = base; m.fork_note = true;                differs(std::move(m), "fork note"); }
    { Message m = base; m.checkpoint_id = CheckpointId{"c"}; differs(std::move(m), "checkpoint id"); }
    { Message m = base; m.timestamp += std::chrono::seconds{1};
                                                          differs(std::move(m), "timestamp"); }
    {
        Message m = base;
        Message::ThinkingBlock b; b.text = "tb";
        m.thinking_blocks.push_back(std::move(b));
        differs(std::move(m), "thinking block");
    }
    {
        Message m = base;
        ToolUse tc;
        tc.id = ToolCallId{"t1"}; tc.name = ToolName{"read"};
        tc.status = ToolUse::Done{{}, {}, "out"};
        m.tool_calls.push_back(std::move(tc));
        differs(std::move(m), "tool call added");
    }

    // A settling tool call: same call, different status/output each time.
    // This is the per-round change the tail save exists to catch.
    {
        Message running = base;
        ToolUse tc;
        tc.id = ToolCallId{"t1"}; tc.name = ToolName{"bash"};
        tc.status = ToolUse::Running{};
        running.tool_calls.push_back(tc);

        Message done = base;
        tc.status = ToolUse::Done{{}, {}, "finished"};
        done.tool_calls.push_back(tc);

        CHECK_MESSAGE(debug_message_fingerprint(running) != debug_message_fingerprint(done),
                      "a tool call settling must move the fingerprint");
    }

    // A one-byte edit at EVERY offset of a long body, including deep inside
    // the 32-byte lane loop and in the ragged tail after it.
    {
        const std::string body(4096, 'a');
        Message m = base; m.text = body;
        const std::uint64_t h = debug_message_fingerprint(m);
        int missed = 0;
        for (std::size_t i = 0; i < body.size(); ++i) {
            Message edited = base;
            edited.text = body;
            edited.text[i] = 'b';
            if (debug_message_fingerprint(edited) == h) ++missed;
        }
        CHECK_MESSAGE(missed == 0, "a one-byte edit was invisible to the fingerprint");
    }

    // Transposed content: same bytes, different order. A lane fold that
    // ignored position would collide here.
    {
        Message a = base; a.text = std::string(32, 'x') + std::string(32, 'y');
        Message b = base; b.text = std::string(32, 'y') + std::string(32, 'x');
        CHECK_MESSAGE(debug_message_fingerprint(a) != debug_message_fingerprint(b),
                      "reordered content must not collide");
    }

    // Identical content fingerprints identically — otherwise every save
    // would rewrite the whole thread.
    {
        Message a = base, b = base;
        CHECK(debug_message_fingerprint(a) == debug_message_fingerprint(b));
    }
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

namespace {
void retitle(const Thread& t, std::string title) {
    persistence::rename_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id,
                               std::move(title));
    persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());
}
} // namespace

TEST_CASE("rename: retitles a log thread and leaves the rest alone") {
    Thread t = make_thread("inc-rename", 5);
    t.updated_at = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);
    const std::string log_before = slurp(log_path(t));

    retitle(t, "a better name");

    auto got = load(t);
    CHECK(got.title == "a better name");
    check_same(t, got);
    // Kept, so the row does not jump to the top of the picker.
    CHECK(got.updated_at == t.updated_at);
    CHECK_MESSAGE(slurp(log_path(t)) == log_before, "a rename never touches the log");

    // The picker's directory walk sees it too, not just the by-id load.
    bool listed = false;
    for (const auto& meta : persistence::load_all_threads(::agentty::IoAccess::grant(), agtest::cap()))
        if (meta.id == t.id) { listed = true; CHECK(meta.title == "a better name"); }
    CHECK(listed);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("rename: a save queued just before does not undo it") {
    Thread t = make_thread("inc-rename-queued", 4);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    save(t);

    // Queue a save built with the OLD title (what ^N / Alt+arrow do for the
    // thread being left), then rename at once. Whether the writer has picked
    // the save up yet is up to it; either way the rename must win.
    t.messages.push_back(make_msg(4));
    persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), t);
    retitle(t, "renamed while queued");

    auto got = load(t);
    CHECK(got.title == "renamed while queued");
    CHECK(got.messages.size() == 5u);

    // And the writer still knows the log: the next save is a tail append.
    mark_head(t);
    t.messages.push_back(make_msg(5));
    t.title = "renamed while queued";
    save(t);
    CHECK_MESSAGE(head_marked(t), "history before the change was not rewritten");
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("rename: a legacy thread is retitled and moved to the log") {
    Thread t = make_thread("inc-rename-legacy", 3);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    {
        auto j = persistence::thread_meta_to_json(t);
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : t.messages) msgs.push_back(persistence::message_to_json(m));
        j["messages"] = std::move(msgs);
        REQUIRE(persistence::write_json_atomic(::agentty::IoAccess::grant(),
            persistence::threads_dir() / (t.id.value + ".json"), j.dump(2)));
    }

    retitle(t, "legacy renamed");

    auto got = load(t);
    CHECK(got.title == "legacy renamed");
    check_same(t, got);
    CHECK(fs::exists(log_path(t)));
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("delete: a save queued just before does not bring the thread back") {
    Thread t = make_thread("inc-delete-queued", 4);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);

    // A thread this process has not saved yet goes to the writer WHOLE: what
    // ^N queues for a thread just opened from the picker. ^D ^D on it right
    // after must win, whether the writer has started that save or not.
    persistence::save_thread(::agentty::IoAccess::grant(), agtest::cap(), t);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    persistence::flush_pending_saves(::agentty::IoAccess::grant(), agtest::cap());

    std::error_code ec;
    CHECK_FALSE(fs::exists(log_path(t), ec));
    CHECK_FALSE(fs::exists(persistence::threads_dir() / (t.id.value + ".meta.json"), ec));
    CHECK_FALSE(fs::exists(persistence::threads_dir() / (t.id.value + ".json"), ec));
    for (const auto& meta : persistence::load_all_threads(::agentty::IoAccess::grant(), agtest::cap()))
        CHECK_MESSAGE(meta.id != t.id, "no ghost row in the picker");

    // A later save of the same id is a new thread: written whole, not a
    // tail append against the log that was deleted.
    Thread again = make_thread("inc-delete-queued", 2);
    save(again);
    auto got = load(again);
    CHECK(got.messages.size() == 2u);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
}

TEST_CASE("rename: an unknown id writes nothing") {
    const Thread t = make_thread("inc-rename-missing", 0);
    persistence::delete_thread(::agentty::IoAccess::grant(), agtest::cap(), t.id);
    retitle(t, "ghost");
    std::error_code ec;
    CHECK_FALSE(fs::exists(persistence::threads_dir() / (t.id.value + ".meta.json"), ec));
    CHECK_FALSE(fs::exists(log_path(t), ec));
}

TEST_CASE("clean_title: one safe line, at most kTitleMaxBytes") {
    using persistence::clean_title;
    CHECK(clean_title("  two\nlines\there  ") == "two lines here");
    CHECK(clean_title("\x1b[31mred") == "[31mred");      // ESC dropped
    CHECK(clean_title("a\xc2\x9b" "b") == "ab");          // C1 CSI dropped
    CHECK(clean_title("   ").empty());
    // Cut on a code-point boundary: 59 ASCII bytes, then a 3-byte em dash
    // that would end at byte 62.
    const std::string long_in = std::string(59, 'x') + "\xe2\x80\x94";
    const std::string out = clean_title(long_in);
    CHECK(out == std::string(59, 'x'));
    CHECK(out.size() <= persistence::kTitleMaxBytes);
    // Kept whole when it fits.
    CHECK(clean_title("caf\xc3\xa9") == "caf\xc3\xa9");
}
