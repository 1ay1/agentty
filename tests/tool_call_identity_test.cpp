// tool_call_identity_test — WHO does this chunk belong to?
//
// Every case here is a shape some provider actually sends, taken from a bug
// report in this project or another harness. The point of the seam is that
// a new report becomes four lines rather than an SSE fixture: no HTTP, no
// JSON, no event plumbing — just "the chunk said this, which call is it?".
//
// The rule being pinned: identity is (id, index) JOINTLY, and when a chunk
// carries neither, attribution FAILS rather than guesses. A wrong guess
// appends one call's bytes to another and yields JSON that still parses, so
// the corruption is silent and reaches a tool invocation. An explicit
// failure is one visibly broken turn. Nothing about the wire lets you avoid
// choosing between those.

#include <string>

#include "agtest.hpp"

#include "agentty/provider/wire/tool_calls.hpp"

using namespace agentty::provider::wire;

namespace {

// Shorthand: a chunk carrying both halves, id only, index only, or neither.
ChunkIdentity both(std::string_view id, std::size_t ix) { return {id, ix}; }
ChunkIdentity id_only(std::string_view id)              { return {id, std::nullopt}; }
ChunkIdentity index_only(std::size_t ix)                { return {"", ix}; }
ChunkIdentity nothing()                                 { return {"", std::nullopt}; }

}  // namespace

TEST_CASE("identity: the ordinary case — open by index, continue by index") {
    ToolCallTracker t;

    auto a = t.attribute(both("call_a", 0));
    REQUIRE(a.has_value());
    CHECK(a->is_new);
    CHECK(!a->displaced.has_value());

    // A continuation chunk carries index only. Same call.
    auto b = t.attribute(index_only(0));
    REQUIRE(b.has_value());
    CHECK(!b->is_new);
    CHECK(b->call == a->call);
    CHECK(t.size() == 1);
}

TEST_CASE("identity: parallel calls stay apart") {
    ToolCallTracker t;
    auto a = t.attribute(both("call_a", 0));
    auto b = t.attribute(both("call_b", 1));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->call != b->call);
    CHECK(t.size() == 2);

    // Interleaved continuations land on the right call.
    auto a2 = t.attribute(index_only(0));
    auto b2 = t.attribute(index_only(1));
    REQUIRE(a2.has_value());
    REQUIRE(b2.has_value());
    CHECK(a2->call == a->call);
    CHECK(b2->call == b->call);
}

TEST_CASE("identity: a reused index with a new id is a NEW call") {
    // THE BUG WE SHIPPED. A server finishes the call at index 0 and starts
    // another one there, distinguishing them only by id. Keying on index
    // alone merged them and corrupted both sets of arguments.
    //
    // Zed's ToolCallAccumulator states the rule this test exists for:
    // "either value alone is unreliable because some providers omit
    // indices, reuse indices, or repeat IDs across parallel calls."
    ToolCallTracker t;
    auto a = t.attribute(both("call_a", 0));
    REQUIRE(a.has_value());
    t.at(*a).started = true;

    auto b = t.attribute(both("call_b", 0));
    REQUIRE(b.has_value());
    CHECK(b->is_new);
    CHECK(b->call != a->call);
    // And the displaced call is reported, because the caller MUST close it:
    // an unclosed call leaves a tool_use with no tool_result and hangs the
    // turn. Splitting without closing trades one bug for a worse one.
    REQUIRE(b->displaced.has_value());
    CHECK(*b->displaced == a->call);

    // An anonymous continuation now belongs to the call holding that index,
    // which is the NEW one.
    auto c = t.attribute(index_only(0));
    REQUIRE(c.has_value());
    CHECK(c->call == b->call);
}

TEST_CASE("identity: a known id continues its call even if the index moves") {
    // The id is the stronger handle. A server that renumbers indices
    // mid-stream must not fork a call that has a stable id.
    ToolCallTracker t;
    auto a = t.attribute(both("call_a", 0));
    REQUIRE(a.has_value());

    auto moved = t.attribute(both("call_a", 3));
    REQUIRE(moved.has_value());
    CHECK(!moved->is_new);
    CHECK(moved->call == a->call);
    CHECK(t.size() == 1);

    // And the new index now routes to it.
    auto by_new = t.attribute(index_only(3));
    REQUIRE(by_new.has_value());
    CHECK(by_new->call == a->call);
}

TEST_CASE("identity: id-only providers (no usable index)") {
    // MiniMax, via zed #42584: calls are identified by id and the index
    // field is absent entirely. Two calls must still stay apart.
    ToolCallTracker t;
    auto a = t.attribute(id_only("call_a"));
    auto b = t.attribute(id_only("call_b"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->call != b->call);

    // A repeated id is the SAME call continuing, not a third one.
    auto a2 = t.attribute(id_only("call_a"));
    REQUIRE(a2.has_value());
    CHECK(!a2->is_new);
    CHECK(a2->call == a->call);
    CHECK(t.size() == 2);
}

TEST_CASE("identity: an empty id is absent, not a value") {
    // Several proxies RESTATE id as "" on continuations rather than
    // omitting it (llm-gateway #136). Treating "" as an identity opens a
    // second call whose id is empty — and the end-of-turn sweep skips
    // empty-id calls, so it is never closed and the turn hangs.
    ToolCallTracker t;
    auto a = t.attribute(both("call_a", 0));
    REQUIRE(a.has_value());

    auto b = t.attribute(both("", 0));
    REQUIRE(b.has_value());
    CHECK(!b->is_new);
    CHECK(b->call == a->call);
    CHECK(t.size() == 1);
}

TEST_CASE("identity: no id and no index is legal for a single call") {
    // Some servers send the opening chunk with neither field. With exactly
    // one call in flight there is no ambiguity to resolve.
    ToolCallTracker t;
    auto a = t.attribute(nothing());
    REQUIRE(a.has_value());
    CHECK(a->is_new);

    auto b = t.attribute(nothing());
    REQUIRE(b.has_value());
    CHECK(!b->is_new);
    CHECK(b->call == a->call);
    CHECK(t.size() == 1);
}

TEST_CASE("identity: no id and no index with several calls FAILS") {
    // The load-bearing case. Two calls in flight and a chunk with no
    // evidence: any choice is a coin flip, and the losing side silently
    // corrupts a tool invocation with JSON that still parses.
    //
    // So it is an error value, not a guess. The caller can then say
    // something true — "the server sent an argument fragment we cannot
    // attribute" — instead of dispatching a tool with another call's bytes.
    ToolCallTracker t;
    (void)t.attribute(both("call_a", 0));
    (void)t.attribute(both("call_b", 1));

    auto amb = t.attribute(nothing());
    REQUIRE(!amb.has_value());
    CHECK(amb.error() == AttributionError::Ambiguous);
}

TEST_CASE("identity: reset clears every handle") {
    ToolCallTracker t;
    (void)t.attribute(both("call_a", 0));
    (void)t.attribute(both("call_b", 1));
    CHECK(t.size() == 2);

    t.reset();
    CHECK(t.empty());

    // The index map went with it — index 0 must open a fresh call, not
    // resolve to a stale slot from the previous turn.
    auto a = t.attribute(index_only(0));
    REQUIRE(a.has_value());
    CHECK(a->is_new);
    CHECK(t.size() == 1);
}
