// blob_gc_test — reclaiming payloads no thread references.
//
// The GC deletes files that hold the user's images and tool output, so
// the tests here are mostly about when it must NOT delete:
//
//   * a blob referenced by ANY thread survives, including one shared
//     between two threads (content-addressing makes that the normal case,
//     not an edge case),
//   * every reference key is honoured, including the ones the writer
//     generates dynamically as `<field>_blob`,
//   * an unreadable thread file aborts the whole sweep, because unknown
//     references cannot be assumed absent,
//   * dry_run touches nothing.

#include "agtest.hpp"

#include <agentty/io/blob_gc.hpp>
#include <agentty/io/blob_store.hpp>
#include <agentty/io/persistence.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty;

namespace {

// A private threads dir per test, so cases can't see each other's blobs.
fs::path make_dir(std::string_view name) {
    auto p = fs::temp_directory_path() / "agentty_blob_gc" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p / "blobs", ec);
    return p;
}

// Put a blob directly, bypassing the store's own path resolution, so the
// test controls which directory it lands in.
std::string put_in(const fs::path& dir, const std::string& bytes) {
    const auto name = blobs::name_for(bytes);
    std::ofstream out(dir / "blobs" / name, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return name;
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::size_t count_blobs(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "blobs", ec))
        if (e.is_regular_file(ec)) ++n;
    return n;
}

bool blob_exists(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    return fs::exists(dir / "blobs" / name, ec);
}

} // namespace

TEST_CASE("blob gc: deletes orphans and keeps everything referenced") {
    const auto dir = make_dir("basic");
    const auto kept    = put_in(dir, std::string(100, 'k'));
    const auto orphan1 = put_in(dir, std::string(200, 'a'));
    const auto orphan2 = put_in(dir, std::string(300, 'b'));
    REQUIRE(count_blobs(dir) == 3u);

    write_file(dir / "t1.json", nlohmann::json{
        {"id", "t1"},
        {"messages", nlohmann::json::array({
            nlohmann::json{{"role", "user"},
                           {"images", nlohmann::json::array({
                               nlohmann::json{{"blob", kept}}})}}})}
    }.dump());

    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK(st.scanned_threads == 1u);
    CHECK(st.total_blobs == 3u);
    CHECK(st.referenced  == 1u);
    CHECK(st.deleted     == 2u);
    CHECK(st.bytes_freed == 500u);

    CHECK_MESSAGE(blob_exists(dir, kept), "a referenced blob must survive");
    CHECK_FALSE(blob_exists(dir, orphan1));
    CHECK_FALSE(blob_exists(dir, orphan2));
}

TEST_CASE("blob gc: a blob shared by two threads survives deleting one") {
    // The case that makes per-thread refcounting wrong. Content-addressing
    // means the same screenshot in two threads is ONE file; deleting
    // thread A must not blank the image in thread B.
    const auto dir = make_dir("shared");
    const auto shared = put_in(dir, std::string(400, 's'));

    auto doc = [&](const char* id) {
        return nlohmann::json{
            {"id", id},
            {"messages", nlohmann::json::array({
                nlohmann::json{{"role", "user"},
                               {"images", nlohmann::json::array({
                                   nlohmann::json{{"blob", shared}}})}}})}
        }.dump();
    };
    write_file(dir / "a.json", doc("a"));
    write_file(dir / "b.json", doc("b"));

    // Thread A goes away; B still references the blob.
    std::error_code ec;
    fs::remove(dir / "a.json", ec);

    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK(st.deleted == 0u);
    CHECK_MESSAGE(blob_exists(dir, shared),
                  "a blob another thread still references must survive");
}

TEST_CASE("blob gc: honours every blob reference key") {
    // Some keys are written literally ("blob", "output_blob"); others are
    // generated as `<field>_blob` by put_or_inline, so a hardcoded list
    // would silently stop protecting new ones. Anything ending in _blob
    // counts.
    const auto dir = make_dir("keys");
    const auto img   = put_in(dir, std::string(10, '1'));
    const auto out   = put_in(dir, std::string(20, '2'));
    const auto think = put_in(dir, std::string(30, '3'));
    const auto sig   = put_in(dir, std::string(40, '4'));
    const auto text  = put_in(dir, std::string(50, '5'));
    const auto orph  = put_in(dir, std::string(60, '6'));
    REQUIRE(count_blobs(dir) == 6u);

    write_file(dir / "t.jsonl", nlohmann::json{
        {"role", "assistant"},
        {"images",     nlohmann::json::array({nlohmann::json{{"blob", img}}})},
        {"tool_calls", nlohmann::json::array({nlohmann::json{{"output_blob", out}}})},
        {"thinking_blob", think},
        {"thinking_signature_blob", sig},
        {"thinking_blocks", nlohmann::json::array({
            nlohmann::json{{"text_blob", text}}})}
    }.dump() + "\n");

    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK_MESSAGE(st.deleted == 1u, "only the genuinely unreferenced blob goes");
    for (const auto& n : {img, out, think, sig, text})
        CHECK_MESSAGE(blob_exists(dir, n), "every _blob key must be honoured");
    CHECK_FALSE(blob_exists(dir, orph));
}

TEST_CASE("blob gc: an unreadable thread aborts the sweep") {
    // Unknown references cannot be assumed absent. A corrupt thread file
    // is exactly when the user most needs their payloads left alone.
    const auto dir = make_dir("unreadable");
    const auto orphan = put_in(dir, std::string(100, 'x'));
    write_file(dir / "broken.json", "{ this is not json at all");

    const auto st = blobs::collect_in(dir);
    CHECK_MESSAGE(!st.ran, "the sweep must report that it did not run");
    CHECK(st.unreadable == 1u);
    CHECK(st.deleted == 0u);
    CHECK_MESSAGE(blob_exists(dir, orphan),
                  "nothing may be deleted while any thread is unreadable");
}

TEST_CASE("blob gc: a torn log line aborts the sweep") {
    // Same rule for the log format: a partial final line means this
    // thread's reference set is incomplete.
    const auto dir = make_dir("torn");
    const auto orphan = put_in(dir, std::string(100, 'y'));
    write_file(dir / "t.jsonl",
               "{\"role\":\"user\",\"text\":\"fine\"}\n{\"role\":\"user\",\"te");

    const auto st = blobs::collect_in(dir);
    CHECK_FALSE(st.ran);
    CHECK(st.deleted == 0u);
    CHECK(blob_exists(dir, orphan));
}

TEST_CASE("blob gc: dry_run reports without deleting") {
    const auto dir = make_dir("dry");
    const auto orphan = put_in(dir, std::string(1024, 'z'));
    write_file(dir / "t.json",
               nlohmann::json{{"id", "t"}, {"messages", nlohmann::json::array()}}.dump());

    const auto st = blobs::collect_in(dir, /*dry_run=*/true);
    CHECK(st.ran);
    CHECK_MESSAGE(st.deleted == 1u, "dry run still reports what it would do");
    CHECK(st.bytes_freed == 1024u);
    CHECK_MESSAGE(blob_exists(dir, orphan), "dry run must not delete");
    CHECK(count_blobs(dir) == 1u);
}

TEST_CASE("blob gc: metadata sidecars are scanned too") {
    // <id>.meta.json holds compaction summaries, which go through
    // put_or_inline and can therefore carry a blob reference. Skipping
    // them would delete a summary that is still in use.
    const auto dir = make_dir("meta");
    const auto summary = put_in(dir, std::string(500, 'm'));
    write_file(dir / "t.jsonl", "{\"role\":\"user\",\"text\":\"hi\"}\n");
    write_file(dir / "t.meta.json", nlohmann::json{
        {"id", "t"},
        {"compactions", nlohmann::json::array({
            nlohmann::json{{"up_to_index", 1}, {"summary_blob", summary}}})}
    }.dump());

    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK(st.deleted == 0u);
    CHECK_MESSAGE(blob_exists(dir, summary),
                  "a reference in the metadata sidecar must be honoured");
}

TEST_CASE("blob gc: dangling references are reported, not fatal") {
    // A reference to a blob that isn't there is a real bug, but the GC's
    // job is to notice it rather than to make it worse.
    const auto dir = make_dir("dangling");
    const auto real = put_in(dir, std::string(64, 'r'));
    write_file(dir / "t.json", nlohmann::json{
        {"id", "t"},
        {"messages", nlohmann::json::array({
            nlohmann::json{{"images", nlohmann::json::array({
                nlohmann::json{{"blob", real}},
                nlohmann::json{{"blob", "0000000000000000-ff"}}})}}})}
    }.dump());

    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK(st.dangling == 1u);
    CHECK(st.deleted == 0u);
    CHECK(blob_exists(dir, real));
}

TEST_CASE("blob gc: an empty store is a no-op") {
    const auto dir = make_dir("empty");
    const auto st = blobs::collect_in(dir);
    CHECK(st.ran);
    CHECK(st.total_blobs == 0u);
    CHECK(st.deleted == 0u);
    CHECK(st.scanned_threads == 0u);
}
