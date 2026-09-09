// lazy_image_test — loading a thread must not decode its images, and must
// still be byte-for-byte correct the moment anything asks for them.
//
// Images are the bulk of a big thread on disk (one real 29 MB thread here is
// 17 MB of image payload), and decoding them at load time is pure waste: the
// render path never reads image bytes, only their sizes. So ImageContent
// defers materialisation to the first bytes() call.
//
// That optimisation is only worth anything if it is invisible. These tests
// pin the properties that make it invisible:
//
//   1. a saved → loaded image yields the ORIGINAL bytes (blob and legacy
//      inline-base64 forms alike),
//   2. loading alone does NOT materialise (the whole point),
//   3. copies of an unmaterialised image still resolve correctly,
//   4. re-saving an unmaterialised image preserves the payload — the
//      dangerous case, since a naive writer would serialise empty bytes and
//      silently destroy every image in the thread on the next autosave,
//   5. a missing blob degrades to empty rather than throwing, matching what
//      the old eager loader did.

#include "agtest.hpp"

#include <agentty/domain/conversation.hpp>
#include <agentty/io/persistence.hpp>
#include <agentty/util/base64.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

// Deterministic pseudo-binary payload — includes bytes that are not valid
// UTF-8, so a path that stringifies rather than treats it as bytes breaks.
std::string payload(std::size_t n, unsigned seed = 1) {
    std::string s;
    s.reserve(n);
    unsigned x = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        s.push_back(static_cast<char>(x >> 24));
    }
    return s;
}

Thread thread_with_image(const std::string& bytes) {
    Thread t;
    t.id = ThreadId{"lazyimg" + std::to_string(bytes.size())};
    Message m;
    m.role = Role::User;
    m.text = "look at this";
    m.images.push_back(ImageContent{"image/png", bytes});
    t.messages.push_back(std::move(m));
    return t;
}

} // namespace

TEST_CASE("lazy image: save → load round-trips the exact bytes") {
    const std::string original = payload(64u * 1024u);
    Thread t = thread_with_image(original);
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto loaded = persistence::load_thread_by_id(t.id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages.size() == 1);
    REQUIRE(loaded->messages[0].images.size() == 1);

    const auto& img = loaded->messages[0].images[0];
    CHECK(img.media_type == "image/png");
    CHECK(img.bytes() == original);
}

TEST_CASE("lazy image: loading does NOT materialise the bytes") {
    const std::string original = payload(32u * 1024u, 7);
    Thread t = thread_with_image(original);
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto loaded = persistence::load_thread_by_id(t.id);
    REQUIRE(loaded.has_value());
    const auto& img = loaded->messages[0].images[0];

    // THE point of the change: nothing was decoded yet.
    CHECK(!img.materialised());
    CHECK(!img.source().blob.empty());

    // Asking is what pays for it — and it's correct when we do.
    CHECK(img.bytes() == original);
    CHECK(img.materialised());
    // Idempotent: a second call returns the same bytes, not a re-read.
    CHECK(img.bytes() == original);
}

TEST_CASE("lazy image: a copy of an unmaterialised image still resolves") {
    const std::string original = payload(16u * 1024u, 3);
    Thread t = thread_with_image(original);
    persistence::save_thread(t);
    persistence::flush_pending_saves();

    auto loaded = persistence::load_thread_by_id(t.id);
    REQUIRE(loaded.has_value());

    // Threads are copied around freely (checkpoints, forks, the view cache).
    // A copy taken BEFORE materialisation must carry the source with it.
    ImageContent copy = loaded->messages[0].images[0];
    CHECK(!copy.materialised());
    CHECK(copy.bytes() == original);
}

TEST_CASE("lazy image: re-saving an unmaterialised image keeps the payload") {
    // The dangerous case. Every turn autosaves the thread; if the writer
    // serialised an unmaterialised image by reading its (empty) bytes, the
    // first save after a switch would wipe every image in the thread.
    const std::string original = payload(48u * 1024u, 11);
    Thread t = thread_with_image(original);
    persistence::save_thread(t);
    persistence::flush_pending_saves();
    auto loaded = persistence::load_thread_by_id(t.id);
    REQUIRE(loaded.has_value());
    CHECK(!loaded->messages[0].images[0].materialised());

    // Save it straight back WITHOUT ever touching bytes().
    persistence::save_thread(*loaded);
    persistence::flush_pending_saves();

    auto again = persistence::load_thread_by_id(t.id);
    REQUIRE(again.has_value());
    REQUIRE(again->messages[0].images.size() == 1);
    CHECK_MESSAGE(again->messages[0].images[0].bytes() == original,
                  "re-saving a never-materialised image must preserve it");
}

TEST_CASE("lazy image: legacy inline base64 still loads") {
    // Threads written before the blob store carry `data` (base64) inline.
    // They must keep working, lazily, with no migration.
    const std::string original = payload(4096, 5);
    Thread t;
    t.id = ThreadId{"lazyimg_legacy"};
    Message m;
    m.role = Role::User;
    m.text = "legacy";
    t.messages.push_back(std::move(m));
    // Seed a LEGACY whole-document file directly. save_thread() now
    // migrates to the log format and retires the .json, so it can no
    // longer be used to manufacture the old shape this test is about.
    const auto seed_path = persistence::threads_dir() / (t.id.value + ".json");
    {
        auto j = persistence::thread_meta_to_json(t);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& mm : t.messages)
            arr.push_back(persistence::message_to_json(mm));
        j["messages"] = std::move(arr);
        persistence::write_json_atomic(seed_path, j.dump(2));
    }

    // Hand-write the legacy shape into the saved file.
    const auto path = persistence::threads_dir() / (t.id.value + ".json");
    std::string doc;
    {
        std::ifstream in(path, std::ios::binary);
        doc.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
    }
    const std::string inject =
        "\"images\":[{\"data\":\"" + util::base64_encode(original)
        + "\",\"media_type\":\"image/png\"}],";
    const auto at = doc.find("\"role\"");
    REQUIRE(at != std::string::npos);
    doc.insert(at, inject);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << doc;
    }

    auto loaded = persistence::load_thread_file(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->messages[0].images.size() == 1);
    const auto& img = loaded->messages[0].images[0];
    CHECK(!img.materialised());          // still lazy, just from base64
    CHECK(img.bytes() == original);
}

TEST_CASE("lazy image: a legacy inline image migrates to a blob on save") {
    // Lazy loading alone doesn't shrink a legacy thread: the 17 MB of base64
    // is still tokenized by the JSON parser on every single load. Saving
    // migrates it to the blob store ONCE, after which the thread file is
    // small forever. The bytes must survive that migration untouched.
    const std::string original = payload(8192, 13);
    Thread t;
    t.id = ThreadId{"lazyimg_migrate"};
    Message m;
    m.role = Role::User;
    m.text = "legacy";
    t.messages.push_back(std::move(m));
    // Seed a LEGACY whole-document file directly (see the note above).
    const auto path = persistence::threads_dir() / (t.id.value + ".json");
    {
        auto j = persistence::thread_meta_to_json(t);
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& mm : t.messages)
            arr.push_back(persistence::message_to_json(mm));
        j["messages"] = std::move(arr);
        persistence::write_json_atomic(path, j.dump(2));
    }

    std::string doc;
    {
        std::ifstream in(path, std::ios::binary);
        doc.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
    }
    const std::string b64 = util::base64_encode(original);
    const std::string inject =
        "\"images\":[{\"data\":\"" + b64 + "\",\"media_type\":\"image/png\"}],";
    const auto at = doc.find("\"role\"");
    REQUIRE(at != std::string::npos);
    doc.insert(at, inject);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << doc;
    }
    const auto legacy_size = fs::file_size(path);

    // Load (lazy) then save straight back — the autosave every turn does.
    auto legacy = persistence::load_thread_file(path);
    REQUIRE(legacy.has_value());
    CHECK(!legacy->messages[0].images[0].materialised());
    persistence::save_thread(*legacy);
    persistence::flush_pending_saves();

    // The save rewrote the thread in the LOG format, so the legacy
    // document is gone and the payload now lives in the blob store.
    const auto log_path = persistence::threads_dir() / (t.id.value + ".jsonl");
    REQUIRE(fs::exists(log_path));
    CHECK_FALSE(fs::exists(path));

    std::string after;
    {
        std::ifstream in(log_path, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    CHECK_MESSAGE(after.find(b64) == std::string::npos,
                  "the inline base64 must be replaced by a blob reference");
    CHECK_MESSAGE(fs::file_size(log_path) < legacy_size,
                  "and the thread file must shrink by the payload size");

    // And the image still reads back byte-for-byte.
    auto migrated = persistence::load_thread_by_id(t.id);
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->messages[0].images.size() == 1);
    CHECK(migrated->messages[0].images[0].bytes() == original);
}

TEST_CASE("lazy image: a missing blob degrades to empty, never throws") {
    // Matches the eager loader's old behaviour for a corrupt/absent payload:
    // empty bytes, which every wire path already skips.
    auto img = ImageContent::lazy(
        "image/png", ImageContent::Source{.blob = "definitely-not-a-real-blob",
                                          .b64 = {}});
    CHECK(img.bytes().empty());
}
