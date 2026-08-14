// dup_tool_call_id_test — duplicate tool-call id ingest regression.
//
// Root cause: several OpenAI-compatible gateways mint a DETERMINISTIC id per
// (tool, index) — literally "bash:0" for every bash call on every turn —
// instead of a unique ToolCallId. One agent turn holds several assistant
// messages in the live tail (submit_message appends a fresh placeholder per
// sub-turn), so turn 2's "bash:0" landed beside turn 1's, already Done, and
// every lookup (`with_live_tool`) matched the FIRST one: the result was
// stamped onto the dead card, the real call stayed Pending, and its card
// hung until the 330 s step timeout — on every response with a tool call.
//
// The fix (stream.cpp): `uniquify` renames the NEW call at ingest
// ("bash:0" -> "bash:0#2", ...) and stashes the original on
// `ToolUse::wire_id`, which `find_streaming_tool` falls back to (newest
// carrier) so the wire's own delta / end / result events still route. The
// rewritten id goes back out on the wire in BOTH the assistant tool_call
// and its paired role:"tool" result (all four transports), so the request
// stays self-consistent — and unlike the original, unambiguous.
//
// `with_live_tool` (update/internal.hpp) additionally picks the FIRST
// NON-TERMINAL call over the first call overall for worried callers that
// still address the raw id.
//
// Asserts:
//   1. A tool_use_start replaying a Done call's id is renamed to id#2;
//      the original keeps id + output; wire_id points back at the wire id.
//   2. A NEVER-TERMINATED hanging first call doesn't swallow the replay:
//      the observed result still lands on the NEW (newest wire_id) call,
//      and the delta routes by wire_id to the newest carrier.
//   3. A THIRD start with the same wire id gets id#3; wire routing stays
//      per-newest — all three results land on their own call.
//   4. A fresh unique id is untouched: no rewrite, wire_id stays empty.
//   5. with_live_tool targeted at the RAW wire id picks the first
//      non-terminal carrier — not the first (already-Done) match.

#include <cstdio>
#include <string>
#include <utility>

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

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

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

// sub_turn_start mirrors kick_pending_tools / submit_message: append the
// next sub-turn's assistant placeholder (the real runtime also expires any
// leftover Pending/Approved calls, which the end() events here already
// handled; when they didn't, that's exactly test 2's hanging case).
static void sub_turn_start(Model& m) {
    m.d.current.messages.push_back(asst_placeholder());
}

// ── 1. Impersonated start: newcomer is renamed, original is untouched ──────
static void test_impersonated_start_renamed() {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());

    // Turn T0: bash:0 starts, streams args, executed -> Done "hello".
    m = start(std::move(m), "bash:0", "bash").first;
    m = delta(std::move(m), "bash:0", "{\"command\": \"echo hello\"}").first;
    m = end(std::move(m), "bash:0").first;
    {
        auto& tc0 = m.d.current.messages.back().tool_calls.back();
        check(tc0.id.value == "bash:0", "T0: first call keeps raw id");
        check(tc0.wire_id.empty(),     "T0: first call has no wire_id");
        tc0.status = ToolUse::Done{
            tc0.started_at(), std::chrono::steady_clock::now(), "hello"};
    }

    // Turn T1: gateway re-mints bash:0 for the very next call.
    sub_turn_start(m);
    m = start(std::move(m), "bash:0", "bash").first;
    const auto& calls = m.d.current.messages.back().tool_calls;
    check(calls.size() == 1,                "T1: placeholder holds the new call");
    check(calls[0].id.value == "bash:0#2",  "T1: newcomer renamed to id#2");
    check(calls[0].wire_id.value == "bash:0", "T1: wire_id keeps the raw wire id");

    // The finished call in the previous sub-turn must be untouched.
    const auto& prev = m.d.current.messages[0].tool_calls[0];
    check(prev.id.value == "bash:0", "T1: original id not rewritten");
    check(prev.is_done() && prev.output() == "hello",
          "T1: original result not clobbered");
}

// ── 2. Result routes to the replay, even if the first call hung ────────────
static void test_result_routes_to_renamed_newcomer() {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());

    // T0's call NEVER terminated (the stuck-card bug shape): still Pending.
    m = start(std::move(m), "bash:0", "bash").first;
    m = delta(std::move(m), "bash:0", "{\"command\": \"echo first\"}").first;
    m = end(std::move(m), "bash:0").first;

    // T1: gateway reuses bash:0.
    sub_turn_start(m);
    m = start(std::move(m), "bash:0", "bash").first;
    m = delta(std::move(m), "bash:0", "{\"command\": \"echo second\"}").first;
    m = end(std::move(m), "bash:0").first;

    // The wire addresses its result by the RAW id. It must land on the
    // replay (newest wire_id carrier), not the hung first call.
    m = observed(std::move(m), "bash:0", "world").first;

    const auto& tc_first = m.d.current.messages[0].tool_calls[0];
    const auto& tc_new   = m.d.current.messages[1].tool_calls[0];
    check(tc_new.id.value == "bash:0#2",        "T1: replay carries id#2");
    check(tc_new.is_done(),                     "result resolves the replay");
    check(tc_new.output() == "world",           "output on the replay");
    check(!tc_first.is_done(),                  "hung first call untouched");
    check(tc_first.args.value("command", "") == "echo first",
          "first call keeps its own args");
    check(tc_new.args.value("command", "") == "echo second",
          "delta routed by wire_id to the replay");
}

// ── 3. Sequential collisions: a third bash:0 renames again ─────────────────
static void test_sequential_collisions() {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());

    m = start(std::move(m), "bash:0", "bash").first;
    m = observed(std::move(m), "bash:0", "r1").first;
    sub_turn_start(m);
    m = start(std::move(m), "bash:0", "bash").first;
    m = observed(std::move(m), "bash:0", "r2").first;
    sub_turn_start(m);
    m = start(std::move(m), "bash:0", "bash").first;
    m = observed(std::move(m), "bash:0", "r3").first;

    const auto& c0 = m.d.current.messages[0].tool_calls[0];
    const auto& c1 = m.d.current.messages[1].tool_calls[0];
    const auto& c2 = m.d.current.messages[2].tool_calls[0];
    check(c1.id.value == "bash:0#2", "collision #2 renamed");
    check(c2.id.value == "bash:0#3", "collision #3 renamed");
    check(c0.is_done() && c0.output() == "r1", "result 1 stayed on first call");
    check(c1.is_done() && c1.output() == "r2", "result 2 routed to id#2");
    check(c2.is_done() && c2.output() == "r3", "result 3 routed to id#3");
}

// ── 4. A genuinely unique id is untouched ──────────────────────────────────
static void test_unique_id_not_rewritten() {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());
    m = start(std::move(m), "bash:0", "bash").first;
    sub_turn_start(m);
    m = start(std::move(m), "call_abc123", "bash").first;
    const auto& tc = m.d.current.messages.back().tool_calls[0];
    check(tc.id.value == "call_abc123", "unique id kept verbatim");
    check(tc.wire_id.empty(),          "no wire_id on the happy path");
}

// ── 5. with_live_tool prefers the first NON-TERMINAL carrier ───────────────
static void test_with_live_tool_prefers_non_terminal() {
    Model m;
    m.d.current.messages.push_back(asst_placeholder());

    // Hand-build the pre-fix collision shape: two calls sharing one raw id,
    // the first already Done, the second still Pending.
    ToolUse done_tc;
    done_tc.id   = ToolCallId{"bash:0"};
    done_tc.name = ToolName{"bash"};
    done_tc.args = {{ "command", "echo first" }};
    done_tc.status = ToolUse::Done{{}, std::chrono::steady_clock::now(), "hi"};
    ToolUse pending_tc = done_tc;
    pending_tc.args    = {{ "command", "echo second" }};
    pending_tc.status  = ToolUse::Pending{std::chrono::steady_clock::now()};
    m.d.current.messages.back().tool_calls.push_back(std::move(done_tc));

    sub_turn_start(m);
    m.d.current.messages.back().tool_calls.push_back(std::move(pending_tc));

    bool hit_pending = false;
    with_live_tool(m, ToolCallId{"bash:0"}, [&](ToolUse& t) {
        hit_pending = t.is_pending();
        t.status = ToolUse::Done{
            t.started_at(), std::chrono::steady_clock::now(), "world"};
    });
    check(hit_pending, "mutation landed on the Pending call, not the Done one");
    check(m.d.current.messages[0].tool_calls[0].output() == "hi",
          "Done call's original output preserved");
    check(m.d.current.messages[1].tool_calls[0].output() == "world",
          "Pending call received the new result");
}

int main() {
    test_impersonated_start_renamed();
    test_result_routes_to_renamed_newcomer();
    test_sequential_collisions();
    test_unique_id_not_rewritten();
    test_with_live_tool_prefers_non_terminal();
    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
