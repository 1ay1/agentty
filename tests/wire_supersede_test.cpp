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

// ── paging a big file ────────────────────────────────────────────────────
//
// THE SECOND BUG, found by mining 40 recent threads: 121 of 3323 reads came
// back as a pointer, and the big-file audits were the ones that broke. The
// collapse keyed on path alone, so window #2 erased window #1 even though it
// never held those lines. Four subagents reading one 1990-line file all
// reported "my reads returned superseded, audit incomplete".

// A windowed read, with the footer `read` actually prints.
ToolUse window_read(const char* id, const char* path, int lo, int hi,
                    int total, std::string body) {
    ToolUse tc;
    tc.id   = ToolCallId{id};
    tc.name = ToolName{"read"};
    tc.args = {{"path", path}, {"start_line", lo}, {"end_line", hi}};
    body += "\n[showing lines " + std::to_string(lo) + "-" + std::to_string(hi)
          + " of " + std::to_string(total) + "]";
    tc.status = ToolUse::Done{{}, {}, std::move(body)};
    return tc;
}

TEST_CASE("supersede: two different windows of one file both survive") {
    auto msgs = thread_of({
        window_read("c1", "/w/big.cpp", 100, 200, 1990, "the first slice\n"),
        window_read("c2", "/w/big.cpp", 900, 1000, 1990, "the second slice\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK_MESSAGE(dead.empty(),
                  "a read of lines 900-1000 does not carry lines 100-200, so "
                  "it cannot stand in for them");
    CHECK(body_survives(msgs, "the first slice"));
    CHECK(body_survives(msgs, "the second slice"));
}

TEST_CASE("supersede: overlapping windows that don't contain each other both survive") {
    auto msgs = thread_of({
        window_read("c1", "/w/big.cpp", 100, 300, 1990, "slice A\n"),
        window_read("c2", "/w/big.cpp", 200, 400, 1990, "slice B\n"),
    });
    CHECK(wire::superseded_read_ids(msgs).empty());
    CHECK(body_survives(msgs, "slice A"));
}

TEST_CASE("supersede: a containing window collapses the one inside it") {
    // The token win still lands where it's actually sound.
    auto msgs = thread_of({
        window_read("c1", "/w/big.cpp", 150, 160, 1990, "inner\n"),
        window_read("c2", "/w/big.cpp", 100, 200, 1990, "inner plus more\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));
    CHECK_FALSE(dead.contains("c2"));
    CHECK(body_survives(msgs, "inner plus more"));
}

TEST_CASE("supersede: a whole-file read collapses every window before it") {
    // No footer means `read` withheld nothing, so this result holds the lot.
    auto msgs = thread_of({
        window_read("c1", "/w/f.txt", 1, 10, 400, "head\n"),
        window_read("c2", "/w/f.txt", 300, 310, 400, "tail\n"),
        read_call("c3", "/w/f.txt", "the entire file\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));
    CHECK(dead.contains("c2"));
    CHECK_FALSE(dead.contains("c3"));
}

TEST_CASE("supersede: a window does NOT collapse an earlier whole-file read") {
    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "the entire file\n"),
        window_read("c2", "/w/f.txt", 300, 310, 400, "one slice\n"),
    });
    CHECK(wire::superseded_read_ids(msgs).empty());
    CHECK(body_survives(msgs, "the entire file"));
}

TEST_CASE("supersede: an outline neither collapses nor is collapsed") {
    // An outline is a symbol index, not content. It can't stand in for a line
    // range, and a line range doesn't contain it either.
    ToolUse outline;
    outline.id   = ToolCallId{"c2"};
    outline.name = ToolName{"read"};
    outline.args = {{"path", "/w/big.cpp"}};
    outline.status = ToolUse::Done{{}, {},
        "This file is large, so here is its shape.\n\n# Outline of /w/big.cpp\n\n"
        "L10: void f()\nL80: void g()\n"};

    auto msgs = thread_of({
        window_read("c1", "/w/big.cpp", 100, 200, 1990, "a slice\n"),
        std::move(outline),
        window_read("c3", "/w/big.cpp", 100, 200, 1990, "the same slice\n"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK_MESSAGE(!dead.contains("c2"),
                  "a window read must not collapse an outline");
    CHECK_MESSAGE(dead.contains("c1"),
                  "the identical later window still collapses the earlier one "
                  "across the outline");
}

TEST_CASE("supersede: a past-the-end read carries nothing and collapses nothing") {
    ToolUse past;
    past.id   = ToolCallId{"c2"};
    past.name = ToolName{"read"};
    past.args = {{"path", "/w/f.txt"}, {"offset", 9000}};
    past.status = ToolUse::Done{{}, {},
        "[offset 9000 is past the end of the file, which has 400 lines "
        "\xe2\x80\x94 nothing to show. Re-read with an offset \xe2\x89\xa4 400.]"};

    auto msgs = thread_of({
        read_call("c1", "/w/f.txt", "the entire file\n"),
        std::move(past),
    });
    CHECK(wire::superseded_read_ids(msgs).empty());
    CHECK(body_survives(msgs, "the entire file"));
}

TEST_CASE("supersede: an edit collapses every live window of the file") {
    // Range doesn't enter into it: the file changed, so every earlier body is
    // stale whatever slice it held.
    auto msgs = thread_of({
        window_read("c1", "/w/f.txt", 1, 10, 400, "head\n"),
        window_read("c2", "/w/f.txt", 300, 310, 400, "tail\n"),
        edit_call("c3", "/w/f.txt"),
    });

    const auto dead = wire::superseded_read_ids(msgs);
    CHECK(dead.contains("c1"));
    CHECK(dead.contains("c2"));
}

TEST_CASE("supersede: an edit collapses a read spelled with a different path") {
    // The compare is on strings, so the two spellings have to normalise to
    // one. Otherwise the wire serves pre-edit bytes as if they were current.
    auto msgs = thread_of({
        read_call("c1", "/w/sub/../f.txt", "before the edit\n"),
        edit_call("c2", "/w/f.txt"),
    });
    CHECK(wire::superseded_read_ids(msgs).contains("c1"));
}

TEST_CASE("supersede: an unparsable footer falls back to the args") {
    // A thread written by an older build, or a reworded footer. The args still
    // say which slice was asked for.
    ToolUse odd;
    odd.id   = ToolCallId{"c1"};
    odd.name = ToolName{"read"};
    odd.args = {{"path", "/w/f.txt"}, {"start_line", 150}, {"end_line", 160}};
    odd.status = ToolUse::Done{{}, {}, "inner\n[showing lines ??? of 400]"};

    auto msgs = thread_of({
        std::move(odd),
        window_read("c2", "/w/f.txt", 100, 200, 400, "inner plus more\n"),
    });
    CHECK_MESSAGE(wire::superseded_read_ids(msgs).contains("c1"),
                  "args-derived span 150-160 sits inside 100-200");
}

TEST_CASE("supersede: a file that QUOTES the sentinel still counts as a body") {
    // This repo's own wire_supersede.hpp and this test file both contain the
    // pointer text. A substring search called such a read bodyless, which
    // dropped it from the edit-invalidation path entirely -- so a later edit
    // left pre-edit bytes on the wire looking current.
    ToolUse quoting;
    quoting.id   = ToolCallId{"c1"};
    quoting.name = ToolName{"read"};
    quoting.args = {{"path", "/w/wire_supersede.hpp"}};
    quoting.status = ToolUse::Done{{}, {},
        std::string{"constexpr auto kPointer =\n  \""}
        + std::string{wire::kSupersededReadPointer} + "\";\n"};

    CHECK(wire::read_result_has_body(quoting));

    auto msgs = thread_of({std::move(quoting), edit_call("c2", "/w/wire_supersede.hpp")});
    CHECK_MESSAGE(wire::superseded_read_ids(msgs).contains("c1"),
                  "the edit must still invalidate it");
}

TEST_CASE("supersede: the pointer itself is not a body") {
    ToolUse pointer;
    pointer.id   = ToolCallId{"c1"};
    pointer.name = ToolName{"read"};
    pointer.args = {{"path", "/w/f.txt"}};
    pointer.status = ToolUse::Done{{}, {}, std::string{wire::kSupersededReadPointer}};
    CHECK_FALSE(wire::read_result_has_body(pointer));
}

TEST_CASE("supersede: a tail read has no absolute span, so it is left alone") {
    // offset:-50 means "the last 50 lines" — which lines that is depends on a
    // length the args don't carry.
    ToolUse tail;
    tail.id   = ToolCallId{"c1"};
    tail.name = ToolName{"read"};
    tail.args = {{"path", "/w/log.txt"}, {"offset", -50}};
    tail.status = ToolUse::Done{{}, {}, "the last fifty\n[showing lines ? of ?]"};

    auto msgs = thread_of({
        std::move(tail),
        window_read("c2", "/w/log.txt", 1, 2000, 9000, "the head\n"),
    });
    CHECK(wire::superseded_read_ids(msgs).empty());
    CHECK(body_survives(msgs, "the last fifty"));
}
