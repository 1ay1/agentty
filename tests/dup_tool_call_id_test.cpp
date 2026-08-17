// dup_tool_call_id_test — duplicate tool-call id ingest regression.
//
// Root cause: several OpenAI-compatible gateways mint a DETERMINISTIC id per
// (tool, index) — literally "bash:0" for every bash call on every turn —
// instead of a unique ToolCallId. One agent turn holds several assistant
// messages in the live tail (kick_pending_tools appends a fresh placeholder
// per sub-turn), so turn 2's "bash:0" lands beside turn 1's, already Done.
// The old `with_live_tool` matched the FIRST id in the whole live tail: turn
// 2's result was stamped onto turn 1's dead card, the real call stayed
// Pending, and its card hung until the 330 s step timeout — on every response
// with a tool call.
//
// The fix (update/internal.hpp): `with_live_tool` prefers the first
// NON-TERMINAL call carrying the id, falling back to a terminal one only when
// no live call has it. Result/progress/timeout/permission routing all go
// through `with_live_tool`, so this alone lands each event on the correct
// (live) card even when an earlier sub-turn left an identically-id'd Done
// call in the tail.
//
// Streaming assembly (stream.cpp `find_streaming_tool`) needs no special
// casing: it is scoped to messages.back() — the CURRENT sub-turn's assistant
// message — so an earlier sub-turn's identically-id'd call is simply out of
// range and cannot shadow the one being streamed.
//
// Asserts:
//   1. with_live_tool targeted at a duplicated id picks the first
//      NON-TERMINAL carrier — not the first (already-Done) match.
//   2. Full cross-sub-turn flow: turn 1's call is Done; turn 2 replays the
//      same wire id; its streamed args + observed result land on turn 2's
//      live call, and turn 1's Done card keeps its own output.
//   3. find_streaming_tool is isolated to the current sub-turn: a delta for
//      the replayed id never leaks onto the previous sub-turn's call.
//   4. A hanging (never-terminated) first call in the SAME message does not
//      swallow a genuinely distinct later call's result.
//   5. Unique ids route to their own call, untouched.

#include <string>
#include <utility>

#include "agtest.hpp"

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"

using agentty::Message;
using agentty::Model;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using agentty::StreamObservedToolResult;
using agentty::StreamToolUseDelta;
using agentty::StreamToolUseEnd;
using agentty::StreamToolUseStart;
using agentty::app::Step;
using agentty::app::detail::stream_update;
using agentty::app::detail::with_live_tool;
namespace msg = agentty::msg;


static Message asst_placeholder() {
    Message m;
    m.role = Role::Assistant;
    return m;
}

static Step start(Model m, const std::string& id, const std::string& name) {
    return stream_update(std::move(m),
                         msg::StreamMsg{StreamToolUseStart{
                             ToolCallId{id}, ToolName{name}}});
}

static Step delta(Model m, const std::string& id, std::string js) {
    return stream_update(std::move(m),
                         msg::StreamMsg{StreamToolUseDelta{
                             ToolCallId{id}, std::move(js)}});
}

static Step end(Model m, const std::string& id) {
    return stream_update(std::move(m),
                         msg::StreamMsg{StreamToolUseEnd{ToolCallId{id}}});
}

static Step observed(Model m, const std::string& id, std::string out) {
    return stream_update(std::move(m),
                         msg::StreamMsg{StreamObservedToolResult{
                             ToolCallId{id}, /*failed=*/false,
                             std::move(out)}});
}

// sub_turn_start mirrors kick_pending_tools: append the next sub-turn's
// assistant placeholder without freezing the prior one, so both live in the
// tail at once — the precondition for the duplicate-id collision.
static void sub_turn_start(Model& m) {
    m.d.current.messages.push_back(asst_placeholder());
}

static ToolUse make_call(const std::string& id, ToolUse::Status st) {
    ToolUse tc;
    tc.id     = ToolCallId{id};
    tc.name   = ToolName{"bash"};
    tc.status = std::move(st);
    return tc;
}

static ToolUse::Done done_status(const char* out) {
    auto now = std::chrono::steady_clock::now();
    return ToolUse::Done{now, now, out};
}

// ── 1. with_live_tool prefers the first non-terminal carrier ───────────────
TEST_CASE("with live tool prefers non terminal") {
    Model m;
    // Two live assistant messages, both carrying "bash:0": the first Done
    // (previous sub-turn), the second Pending (current). The raw lookup must
    // land on the Pending one.
    m.d.current.messages.push_back(asst_placeholder());
    m.d.current.messages.back().tool_calls.push_back(
        make_call("bash:0", done_status("stale")));
    m.d.current.messages.push_back(asst_placeholder());
    m.d.current.messages.back().tool_calls.push_back(
        make_call("bash:0", ToolUse::Pending{}));

    bool hit_terminal = false;
    bool hit_pending  = false;
    with_live_tool(m, ToolCallId{"bash:0"}, [&](ToolUse& tc) {
        if (tc.is_terminal()) hit_terminal = true;
        if (tc.is_pending())  hit_pending  = true;
    });
    check(!hit_terminal, "L1: did not route to the Done card");
    check(hit_pending,   "L1: routed to the live (Pending) card");

    // With the live call gone (all terminal), it may fall back to a terminal
    // match rather than silently no-op.
    m.d.current.messages.back().tool_calls[0].status = done_status("fresh");
    bool any = false;
    with_live_tool(m, ToolCallId{"bash:0"}, [&](ToolUse&) { any = true; });
    check(any, "L1: falls back to a terminal match when none are live");
}

// ── 2. Cross-sub-turn: result lands on the live call, Done card untouched ──
TEST_CASE("result routes across sub turns") {
    Model m;
    // Turn 1: stream bash:0, finish it, drive it Done with its own output.
    m.d.current.messages.push_back(asst_placeholder());
    Step s = start(std::move(m), "bash:0", "bash");
    s = delta(std::move(s.first), "bash:0", "{\"cmd\":\"one\"}");
    s = end(std::move(s.first), "bash:0");
    m = std::move(s.first);
    m.d.current.messages.back().tool_calls[0].status = done_status("first-out");

    // Turn 2: new sub-turn placeholder, same wire id replays.
    sub_turn_start(m);
    s = start(std::move(m), "bash:0", "bash");
    s = delta(std::move(s.first), "bash:0", "{\"cmd\":\"two\"}");
    s = end(std::move(s.first), "bash:0");
    // The observed result for turn 2 must land on turn 2's live call.
    s = observed(std::move(s.first), "bash:0", "second-out");
    m = std::move(s.first);

    const auto& t1 = m.d.current.messages[m.d.current.messages.size() - 2];
    const auto& t2 = m.d.current.messages.back();
    check(t1.tool_calls.size() == 1 && t2.tool_calls.size() == 1,
          "T2: one call in each sub-turn message");
    check(t1.tool_calls[0].output() == "first-out",
          "T2: turn 1's Done card keeps its own output");
    check(t2.tool_calls[0].is_terminal(),
          "T2: turn 2's call settled (did not hang)");
    check(t2.tool_calls[0].output() == "second-out",
          "T2: turn 2's result landed on turn 2's call");
}

// ── 3. find_streaming_tool is scoped to the current sub-turn ───────────────
TEST_CASE("streaming isolated to current sub turn") {
    Model m;
    // Turn 1: a fully-streamed bash:0 with args "{A}".
    m.d.current.messages.push_back(asst_placeholder());
    Step s = start(std::move(m), "bash:0", "bash");
    s = delta(std::move(s.first), "bash:0", "{A}");
    s = end(std::move(s.first), "bash:0");
    m = std::move(s.first);
    const std::string turn1_args = m.d.current.messages.back().tool_calls[0].args_streaming;

    // Turn 2: new placeholder, same id, DIFFERENT args stream.
    sub_turn_start(m);
    s = start(std::move(m), "bash:0", "bash");
    s = delta(std::move(s.first), "bash:0", "{B}");
    m = std::move(s.first);

    const auto& t1 = m.d.current.messages[m.d.current.messages.size() - 2];
    const auto& t2 = m.d.current.messages.back();
    check(t1.tool_calls[0].args_streaming == turn1_args,
          "T3: turn 1's streamed args were NOT mutated by turn 2's delta");
    check(t2.tool_calls[0].args_streaming.find("{B}") != std::string::npos,
          "T3: turn 2's delta landed on turn 2's call");
}

// ── 4. A hanging first call doesn't swallow a distinct later call ──────────
TEST_CASE("hanging first call does not swallow") {
    Model m;
    // One message, two DISTINCT ids (normal parallel calls). The first never
    // terminates; the second gets its own result.
    m.d.current.messages.push_back(asst_placeholder());
    Step s = start(std::move(m), "bash:0", "bash");   // never ended → hangs
    s = start(std::move(s.first), "bash:1", "bash");
    s = end(std::move(s.first), "bash:1");
    s = observed(std::move(s.first), "bash:1", "b1-out");
    m = std::move(s.first);

    const auto& calls = m.d.current.messages.back().tool_calls;
    check(calls.size() == 2, "T4: both parallel calls present");
    check(!calls[0].is_terminal(), "T4: first (hanging) call still live");
    check(calls[1].output() == "b1-out",
          "T4: second call received its own result");
}

// ── 5. Unique ids route to their own call ──────────────────────────────────
TEST_CASE("unique ids untouched") {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());
    Step s = start(std::move(m), "call_a", "bash");
    s = start(std::move(s.first), "call_b", "read");
    s = delta(std::move(s.first), "call_a", "{\"x\":1}");
    s = delta(std::move(s.first), "call_b", "{\"y\":2}");
    m = std::move(s.first);

    const auto& calls = m.d.current.messages.back().tool_calls;
    check(calls.size() == 2, "T5: two distinct calls");
    check(calls[0].id.value == "call_a" && calls[1].id.value == "call_b",
          "T5: unique ids untouched");
    check(calls[0].args_streaming.find("\"x\":1") != std::string::npos,
          "T5: call_a got its own args");
    check(calls[1].args_streaming.find("\"y\":2") != std::string::npos,
          "T5: call_b got its own args");
}
