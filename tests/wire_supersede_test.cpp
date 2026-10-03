// wire_supersede_test.cpp — the read-collapse must never strand the bytes.
//
// Two independent dedup layers meet on the wire:
//
//   fs.cpp          a repeat read of an already-served range answers
//                   "File unchanged since last read" and POINTS at the
//                   earlier result instead of re-sending bytes.
//   superseded_read_ids()
//                   an earlier read whose file was touched again is replaced
//                   on the wire by a one-line pointer to the newer result.
//
// Each is sound alone. Together they used to break the bargain they both
// depend on: read #1 was collapsed because read #2 existed, and read #2 was
// itself only a sentinel — so the file's bytes appeared in NO tool result.
// Reproduced live before the fix; the model's own report was "the two
// sentinels point at each other, and there is no tool result anywhere
// containing the file's text", after which it fell back to `cat`. That is the
// read-loop / shell-fallback users report.
//
// Invariant pinned here: for any path, if the bytes were ever served, at least
// one surviving tool result still carries them.
#include <string>
#include <vector>

#include "agentty/provider/wire_supersede.hpp"
#include "agtest.hpp"

using namespace agentty;
namespace wire = agentty::provider::wire;

namespace {

ToolUse read_call(const char* id, const char* path, std::string output) {
    ToolUse tc;
    tc.id   = ToolCallId{id};
    tc.name = ToolName{"read"};
    tc.args = {{"path", path}};
    tc.status = ToolUse::Done{{}, {}, std::move(output)};
    return tc;
}

ToolUse edit_call(const char* id, const char* path) {
    ToolUse tc;
    tc.id   = ToolCallId{id};
    tc.name = ToolName{"edit"};
    tc.args = {{"path", path}};
    tc.status = ToolUse::Done{{}, {}, std::string{"Edited (1+ 1-)"}};
    return tc;
}

// The sentinel fs.cpp emits for a repeat of an already-served range.
std::string unchanged_sentinel() {
    return "File unchanged since last read. The content from the earlier Read "
           "tool_result in this conversation is still current.";
}

std::vector<Message> thread_of(std::vector<ToolUse> calls) {
    Message m;
    m.role = Role::Assistant;
    m.tool_calls = std::move(calls);
    return {std::move(m)};
}

// THE invariant: some surviving result still carries the bytes.
bool body_survives(const std::vector<Message>& msgs, std::string_view needle) {
    const auto dead = wire::superseded_read_ids(msgs);
    for (const auto& m : msgs)
        for (const auto& tc : m.tool_calls)
            if (!dead.contains(tc.id.value)
                && tc.output().find(needle) != std::string::npos)
                return true;
    return false;
}

}  // namespace

TEST_CASE("supersede: a sentinel-only read never collapses the real one") {
    // THE BUG. Read #1 serves the body; read #2 is answered by the fs dedup
    // with a pointer back to #1. Collapsing #1 leaves the bytes nowhere.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "line 1\nline 2\nline 3\n"),
        read_call("c2", "/w/f.txt", unchanged_sentinel()),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK_MESSAGE(!dead.contains("c1"),
                  "the only result holding the bytes must survive when the "
                  "newer read is just a pointer to it");
    CHECK_MESSAGE(body_survives(msgs, "line 2"),
                  "the file's content must exist in SOME surviving result");
}

TEST_CASE("supersede: a cached read carries its own bytes and may supersede") {
    // The fs layer no longer answers a repeat with a bare pointer -- it
    // replays the text it served under a `[cached …]` tag. So a cache hit is
    // a FULL COPY and must behave like any other read: it supersedes the older
    // result, and the content survives.
    //
    // This is the property the old design could not hold. A pointer is only
    // valid while its target is on the wire, and compaction, this very
    // collapse, and subagent boundaries all invalidate it. Claude Code has the
    // same failure open as #53578 (read loop) and #60684 (stale snapshot);
    // KhazAkar's proposal -- serve the cached value, annotate it -- removes
    // the class rather than adding another TTL heuristic on top of it.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "line 1\nline 2\nline 3\n"),
        read_call("c2", "/w/f.txt",
                  "[cached \xe2\x80\x94 unchanged since your last read of this file]\n"
                  "line 1\nline 2\nline 3\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK_MESSAGE(dead.contains("c1"),
                  "a cached result holds the bytes, so the older copy is "
                  "genuinely redundant and should fade");
    CHECK_FALSE(dead.contains("c2"));
    // THE invariant, and the whole point: the content is still reachable even
    // though the first read was collapsed.
    CHECK_MESSAGE(body_survives(msgs, "line 2"),
                  "collapsing the older read must not strand the content when "
                  "the newer one is a cache hit");
}

TEST_CASE("supersede: a legacy sentinel from an old thread file still protects") {
    // A thread SAVED by a build that emitted the old contentless sentinel
    // rehydrates with that text. The guard stays so those transcripts keep
    // working: the sentinel carries no bytes, so it must not collapse the one
    // result that does.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "line 1\nline 2\nline 3\n"),
        read_call("c2", "/w/f.txt", unchanged_sentinel()),
    });
    CHECK_FALSE(wire::superseded_read_ids(msgs).contains("c1"));
    CHECK(body_survives(msgs, "line 2"));
}

TEST_CASE("supersede: a real re-read still collapses the older one") {
    // The behaviour worth keeping: two reads that both carry bytes means the
    // older body is redundant, so it fades to a pointer. This is the whole
    // point of the collapse and must not regress.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "old contents here\n"),
        read_call("c2", "/w/f.txt", "new contents here\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));
    CHECK_FALSE(dead.contains("c2"));
    CHECK(body_survives(msgs, "new contents"));
}

TEST_CASE("supersede: an edit still collapses the pre-edit read") {
    // An edit genuinely invalidates the old body, whatever it contained, so
    // this collapse is correct even though the edit result carries no file
    // bytes of its own.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "before the edit\n"),
        edit_call("c2", "/w/f.txt"),
    });
    CHECK(wire::superseded_read_ids(msgs).contains("c1"));
}

TEST_CASE("supersede: three reads keep exactly the newest body") {
    // read, sentinel, read. The middle one must not become the live copy and
    // must not collapse the first; the third carries bytes, so by then the
    // first IS redundant.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "first body\n"),
        read_call("c2", "/w/f.txt", unchanged_sentinel()),
        read_call("c3", "/w/f.txt", "third body\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));
    CHECK_FALSE(dead.contains("c3"));
    CHECK(body_survives(msgs, "third body"));
}

TEST_CASE("supersede: a sentinel read of a never-read file strands nothing") {
    // Degenerate but reachable: the fs cache survives across a thread switch,
    // so the first read IN THIS THREAD can be the sentinel. There is nothing
    // to collapse and nothing to claim as live.
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", unchanged_sentinel()),
    });
    CHECK(wire::superseded_read_ids(msgs).empty());
}

TEST_CASE("supersede: different paths never interfere") {
    auto msgs = thread_of({
        read_call("c1", "/w/a.txt", "alpha body\n"),
        read_call("c2", "/w/b.txt", "beta body\n"),
        read_call("c3", "/w/a.txt", "alpha again\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));        // superseded by c3
    CHECK_FALSE(dead.contains("c2"));  // a different file
    CHECK(body_survives(msgs, "beta body"));
}

TEST_CASE("supersede: a failed read is left alone") {
    // Error text never fades — the model needs the whole failure to recover.
    ToolUse bad;
    bad.id   = ToolCallId{"c1"};
    bad.name = ToolName{"read"};
    bad.args = {{"path", "/w/missing.txt"}};
    bad.status = ToolUse::Failed{{}, {}, "file not found: /w/missing.txt"};

    auto msgs = thread_of({std::move(bad),
                           read_call("c2", "/w/missing.txt", "now it exists\n")});
    CHECK_FALSE(wire::superseded_read_ids(msgs).contains("c1"));
}
