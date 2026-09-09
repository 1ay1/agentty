// thread_migration_test — a save moves a thread to the log, and can
// never lose it doing so.
//
// This is the commit that touches user data: saving a thread now writes
// the log and DELETES the legacy <id>.json. A save runs every turn, so a
// bug here is unrecoverable — the file it removes is the only other copy.
//
// The safety argument is verify-before-delete: write the pair, read it
// back off disk, compare, and only then remove the .json. These tests pin
// both halves of that:
//
//   * the happy path really does migrate (and the thread survives it),
//   * every failure path leaves the legacy document exactly where it was.

#include "agtest.hpp"

#include <agentty/io/persistence.hpp>
#include <agentty/io/thread_log.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

Thread make_thread(std::string id, std::size_t n_messages) {
    Thread t;
    t.id    = ThreadId{std::move(id)};
    t.title = "migration subject";
    for (std::size_t i = 0; i < n_messages; ++i) {
        Message m;
        m.role = (i % 2 == 0) ? Role::User : Role::Assistant;
        m.id   = MessageId{"m" + std::to_string(i)};
        m.text = "message " + std::to_string(i)
               + " with \"quotes\", a \\ backslash, \na newline, "
                 "and \xE2\x80\x94 an em dash";
        t.messages.push_back(std::move(m));
    }
    return t;
}

fs::path legacy_path(const Thread& t) {
    return persistence::threads_dir() / (t.id.value + ".json");
}
fs::path log_path(const Thread& t) {
    return persistence::threads_dir() / (t.id.value + ".jsonl");
}

// Write a thread in the OLD format only, as it would exist on disk before
// this change ever ran.
void seed_legacy(const Thread& t) {
    auto j = persistence::thread_meta_to_json(t);
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : t.messages) msgs.push_back(persistence::message_to_json(m));
    j["messages"] = std::move(msgs);
    persistence::write_json_atomic(legacy_path(t), j.dump(2));
}

void wipe(const Thread& t) {
    std::error_code ec;
    fs::remove(legacy_path(t), ec);
    if (auto log = ThreadLog::open(t.id)) log->remove();
}

} // namespace

TEST_CASE("migration: saving a legacy thread moves it to the log") {
    auto t = make_thread("mig_happy", 25);
    wipe(t);
    seed_legacy(t);
    REQUIRE(fs::exists(legacy_path(t)));
    REQUIRE_FALSE(fs::exists(log_path(t)));

    persistence::save_thread(t);
    persistence::flush_pending_saves();

    CHECK_MESSAGE(fs::exists(log_path(t)),
                  "the save must write the log");
    CHECK_MESSAGE(!fs::exists(legacy_path(t)),
                  "and retire the legacy document once it verifies");

    // The thread must come back intact through the normal store seam.
    auto back = persistence::load_thread_by_id(t.id);
    REQUIRE(back.has_value());
    CHECK(back->id.value  == t.id.value);
    CHECK(back->title     == t.title);
    REQUIRE(back->messages.size() == t.messages.size());
    for (std::size_t i = 0; i < t.messages.size(); ++i) {
        CHECK(back->messages[i].id.value == t.messages[i].id.value);
        CHECK(back->messages[i].text     == t.messages[i].text);
        CHECK(back->messages[i].role     == t.messages[i].role);
    }
    wipe(t);
}

TEST_CASE("migration: a thread with no legacy file just writes the log") {
    // The new-thread case. Nothing to retire, nothing to lose.
    auto t = make_thread("mig_fresh", 5);
    wipe(t);

    persistence::save_thread(t);
    persistence::flush_pending_saves();

    CHECK(fs::exists(log_path(t)));
    CHECK_FALSE(fs::exists(legacy_path(t)));
    auto back = persistence::load_thread_by_id(t.id);
    REQUIRE(back.has_value());
    CHECK(back->messages.size() == t.messages.size());
    wipe(t);
}

TEST_CASE("migration: saving again is idempotent") {
    // Every turn saves. The second save has no legacy file to remove and
    // must not corrupt or duplicate anything.
    auto t = make_thread("mig_twice", 10);
    wipe(t);
    seed_legacy(t);

    persistence::save_thread(t);
    persistence::flush_pending_saves();
    REQUIRE(fs::exists(log_path(t)));

    t.messages.push_back([] {
        Message m;
        m.role = Role::User;
        m.id   = MessageId{"m_extra"};
        m.text = "one more turn";
        return m;
    }());
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto back = persistence::load_thread_by_id(t.id);
    REQUIRE(back.has_value());
    CHECK_MESSAGE(back->messages.size() == 11u,
                  "the second save must replace history, not append to it");
    CHECK(back->messages.back().text == "one more turn");
    wipe(t);
}

TEST_CASE("migration: Smart Mode routing cards are not persisted") {
    // They are view-only telemetry, excluded from the wire and from disk.
    // The verification step compares against the PERSISTABLE messages, so
    // if that filter were wrong the save would fail verification and never
    // migrate — this pins the intended behaviour either way.
    auto t = make_thread("mig_smart", 6);
    Message card;
    card.role          = Role::Assistant;
    card.id            = MessageId{"routing"};
    card.text          = "routed to a cheaper model";
    card.smart_routing = true;
    t.messages.insert(t.messages.begin() + 3, card);
    wipe(t);

    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto back = persistence::load_thread_by_id(t.id);
    REQUIRE(back.has_value());
    CHECK_MESSAGE(back->messages.size() == 6u,
                  "the routing card must not be written to disk");
    for (const auto& m : back->messages)
        CHECK(m.id.value != "routing");
    // And it still migrated, i.e. verification passed.
    CHECK(fs::exists(log_path(t)));
    CHECK_FALSE(fs::exists(legacy_path(t)));
    wipe(t);
}

TEST_CASE("migration: an unwritable log keeps the legacy document") {
    // The failure that matters. If the log cannot be written, the save
    // MUST fall back and leave the old file intact — losing a turn is bad,
    // losing the thread is unacceptable.
    //
    // A directory sitting where the log file belongs makes every write to
    // that path fail, without needing permissions games that behave
    // differently as root or on Windows.
    auto t = make_thread("mig_blocked", 8);
    wipe(t);
    seed_legacy(t);

    std::error_code ec;
    fs::create_directories(log_path(t), ec);
    REQUIRE(fs::is_directory(log_path(t)));

    persistence::save_thread(t);
    persistence::flush_pending_saves();

    CHECK_MESSAGE(fs::exists(legacy_path(t)),
                  "a failed log write must leave the legacy document alone");

    // And the thread still loads, through the legacy path.
    auto back = persistence::load_thread_by_id(t.id);
    REQUIRE(back.has_value());
    CHECK(back->messages.size() == t.messages.size());

    fs::remove_all(log_path(t), ec);
    wipe(t);
}

TEST_CASE("migration: deleting a migrated thread removes every file") {
    auto t = make_thread("mig_delete", 4);
    wipe(t);
    persistence::save_thread(t);
    persistence::flush_pending_saves();
    REQUIRE(fs::exists(log_path(t)));

    persistence::delete_thread(t.id);

    CHECK_FALSE(fs::exists(log_path(t)));
    CHECK_FALSE(fs::exists(persistence::threads_dir() / (t.id.value + ".ofs")));
    CHECK_FALSE(fs::exists(persistence::threads_dir() / (t.id.value + ".meta.json")));
    CHECK_FALSE(persistence::load_thread_by_id(t.id).has_value());
}

TEST_CASE("migration: the picker lists a migrated thread exactly once") {
    // load_all_threads walks *.json. A migrated thread has only
    // <id>.meta.json, and mid-migration both can exist — so this is where
    // a thread would either vanish from the list or appear twice.
    auto a = make_thread("mig_listed", 3);
    wipe(a);
    seed_legacy(a);
    persistence::save_thread(a);
    persistence::flush_pending_saves();

    const auto all = persistence::load_all_threads();
    std::size_t seen = 0;
    for (const auto& t : all) if (t.id.value == a.id.value) ++seen;
    CHECK_MESSAGE(seen == 1u, "a migrated thread must be listed exactly once");

    // And with its real title, not a stem-derived artefact.
    for (const auto& t : all)
        if (t.id.value == a.id.value) CHECK(t.title == a.title);
    wipe(a);
}
