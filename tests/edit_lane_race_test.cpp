// edit_lane_race_test — concurrent edits of ONE file must not lose each other.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// edit / apply_patch / write are read-modify-write: read the bytes, compute
// the change, write_atomic the result. The parent's tool scheduler keeps its
// own calls on one path apart, but it is not the only caller. `task`
// subagents schedule as {ReadFs, Net}, so several run at once and beside the
// parent's writers, and each dispatches its tools straight through
// DynamicDispatch with no scheduler. Two edits of the same file then
// interleaved as read A, read B, write A, write B: B's write was computed
// from the bytes before A and silently dropped A's change, while BOTH calls
// reported success.
//
// The fix is a per-path write lane in the tool bridge. This test drives the
// real `edit` tool from many threads at the same file, each replacing its own
// distinct marker. With the lane every replacement survives; without it some
// of them are lost (the file ends with markers still in it).
#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* tag) {
    auto dir = agentty::tools::util::workspace_root() / "build" / "test-scratch";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / ("edit_lane_" + std::string{tag} + ".txt");
}

void put(const fs::path& p, std::string_view body) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

std::string marker(int i) { return "TODO_SLOT_" + std::to_string(i) + "_END"; }
std::string done(int i)   { return "DONE_SLOT_" + std::to_string(i) + "_END"; }

} // namespace

TEST_CASE("edit lane: concurrent edits of one file each land, none is lost") {
    constexpr int kThreads = 12;
    const auto p = scratch("same_file");

    std::string body;
    for (int i = 0; i < kThreads; ++i) body += "line " + marker(i) + "\n";
    put(p, body);

    // Line up the threads so their reads overlap as much as possible.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> ok{0};
    std::vector<std::thread> ws;
    ws.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ws.emplace_back([&, i] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            auto r = agentty::tool::DynamicDispatch::execute(
                agtest::cap(), "edit",
                json{{"path", p.string()},
                     {"edits", json::array({json{{"old_text", marker(i)},
                                                 {"new_text", done(i)}}})}});
            if (r) ok.fetch_add(1);
        });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    go.store(true);
    for (auto& w : ws) w.join();

    const auto after = get(p);
    CHECK(ok.load() == kThreads);
    for (int i = 0; i < kThreads; ++i) {
        CHECK_MESSAGE(after.find(done(i)) != std::string::npos,
                      "edit ", i, " reported success but its change was lost");
        CHECK_MESSAGE(after.find(marker(i)) == std::string::npos,
                      "marker ", i, " still present: an edit was overwritten");
    }
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("edit lane: edits of DIFFERENT files are not serialised into failure") {
    constexpr int kThreads = 6;
    std::vector<fs::path> files;
    for (int i = 0; i < kThreads; ++i) {
        files.push_back(scratch(("distinct_" + std::to_string(i)).c_str()));
        put(files.back(), "x " + marker(i) + "\n");
    }
    std::atomic<int> ok{0};
    std::vector<std::thread> ws;
    for (int i = 0; i < kThreads; ++i)
        ws.emplace_back([&, i] {
            auto r = agentty::tool::DynamicDispatch::execute(
                agtest::cap(), "edit",
                json{{"path", files[i].string()},
                     {"edits", json::array({json{{"old_text", marker(i)},
                                                 {"new_text", done(i)}}})}});
            if (r) ok.fetch_add(1);
        });
    for (auto& w : ws) w.join();
    CHECK(ok.load() == kThreads);
    for (int i = 0; i < kThreads; ++i) {
        CHECK(get(files[i]).find(done(i)) != std::string::npos);
        std::error_code ec;
        fs::remove(files[i], ec);
    }
}

// ── The lane table itself (write_lanes.hpp) ─────────────────────────────────
#include "agentty/tool/write_lanes.hpp"

namespace wl = agentty::tools::write_lanes;

TEST_CASE("write lanes: a writer that names no file holds the whole workspace") {
    agentty::tools::CallContext ctx;
    // git_commit / an MCP tool: writes, names nothing.
    const auto any = wl::lanes_for("git_commit", json::object(), ctx);
    REQUIRE(any.size() == 1);
    CHECK(any[0] == std::string{wl::kWholeWorkspace});
    // It collides with every file lane, so it runs alone among writers.
    CHECK(wl::overlap(any[0], "/ws/src/a.c"));
    CHECK(wl::overlap("/ws/src/a.c", any[0]));
    // A non-string or empty path is "unsure", which is also the whole lane.
    CHECK(wl::lanes_for("edit", json{{"path", 7}}, ctx)[0] == std::string{wl::kWholeWorkspace});
    CHECK(wl::lanes_for("edit", json{{"path", ""}}, ctx)[0] == std::string{wl::kWholeWorkspace});
}

TEST_CASE("write lanes: overlap is by path prefix, on a separator boundary") {
    CHECK(wl::overlap("/ws/src", "/ws/src/a.c"));
    CHECK(wl::overlap("/ws/src/a.c", "/ws/src/a.c"));
    CHECK_FALSE(wl::overlap("/ws/src", "/ws/srcfoo/a.c"));
    CHECK_FALSE(wl::overlap("/ws/a.c", "/ws/b.c"));
}

TEST_CASE("write lanes: a call cancelled while queued gives up, holds nothing") {
    agentty::tools::CallContext ctx;
    const auto lanes = wl::lanes_for("edit", json{{"path", scratch("cancel").string()}}, ctx);
    std::stop_source cancel;
    agentty::tools::CallContext queued_ctx;
    queued_ctx.cancel.push_back(cancel.get_token());

    wl::Hold first{agtest::cap(), lanes, ctx};
    REQUIRE(first.held());
    std::atomic<bool> gave_up{false};
    std::thread waiter([&] {
        wl::Hold second{agtest::cap(), lanes, queued_ctx};
        gave_up = !second.held();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // let it queue
    cancel.request_stop();
    waiter.join();
    CHECK(gave_up.load());
    CHECK(wl::held_count(agtest::cap()) == lanes.size());   // only `first`'s
}

TEST_CASE("write lanes: crossing claims (move A->B beside B->A) never deadlock") {
    agentty::tools::CallContext ctx;
    const auto a = scratch("cross_a").string();
    const auto b = scratch("cross_b").string();
    const auto ab = wl::lanes_for("move", json{{"source", a}, {"destination", b}}, ctx);
    const auto ba = wl::lanes_for("move", json{{"source", b}, {"destination", a}}, ctx);
    std::atomic<int> done{0};
    std::vector<std::thread> ws;
    for (int i = 0; i < 16; ++i)
        ws.emplace_back([&, i] {
            for (int k = 0; k < 50; ++k) {
                wl::Hold h{agtest::cap(), (i % 2) ? ab : ba, ctx};
                REQUIRE(h.held());
            }
            done.fetch_add(1);
        });
    for (auto& w : ws) w.join();   // a deadlock hangs here (ctest TIMEOUT)
    CHECK(done.load() == 16);
    CHECK(wl::held_count(agtest::cap()) == 0);
}
