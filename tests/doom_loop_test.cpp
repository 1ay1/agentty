// doom_loop_test — agent_loop_should_break (cmd_factory.cpp).
//
// Weak local models (qwen2.5-coder, codellama) fall into non-converging tool
// loops: pick the wrong tool for a goal, get an error, re-issue a near-
// identical call forever. With no native completion signal the main agent loop
// would spin until the user hits Esc — the symptom behind the "tool usage is
// fucked" reports. agent_loop_should_break is the pure circuit breaker that
// the continuation point in kick_pending_tools consults before spending
// another model completion. Two triggers:
//   (1) REPEAT  — same (tool,args) failing call >= 3 times.
//   (2) RUNAWAY — >= 25 tool-call turns in one run with no text answer.
//
// Asserts the breaker FIRES on a genuine doom loop and STAYS QUIET on healthy
// progress, on success, and across a User-turn boundary (each run is fresh).

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"

using agentty::Message;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using agentty::app::cmd::agent_loop_should_break;
using nlohmann::json;

namespace {


enum class Term { Done, Failed, Rejected };

ToolUse call(const std::string& name, json args, Term t) {
    static int seq = 0;
    ToolUse tc;
    tc.id   = ToolCallId{"call_" + std::to_string(seq++)};
    tc.name = ToolName{name};
    tc.args = std::move(args);
    if (t == Term::Done) tc.status = ToolUse::Done{{}, {}, "ok"};
    else if (t == Term::Failed) tc.status = ToolUse::Failed{{}, {}, "no such file"};
    else tc.status = ToolUse::Rejected{};
    return tc;
}

Message asst_call(const std::string& name, json args, Term t) {
    Message m;
    m.role = Role::Assistant;
    m.tool_calls.push_back(call(name, std::move(args), t));
    return m;
}

Message asst_text(std::string s) {
    Message m;
    m.role = Role::Assistant;
    m.text = std::move(s);
    return m;
}

Message user(std::string s = "go") {
    Message m;
    m.role = Role::User;
    m.text = std::move(s);
    return m;
}

}  // namespace (helpers)

// ── 1. REPEAT: same failing call 3x → break ─────────────────────────
TEST_CASE("repeat failing call breaks") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "https://x.com/jokes"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));

    auto brk = agent_loop_should_break(msgs);
    check(brk.has_value(), "3x identical failing read → breaks");
    check(brk && brk->reason.find("read") != std::string::npos,
          "break reason names the offending tool");
}

// ── 2. Two failures is below the limit → no break (give it a chance) ─────────
TEST_CASE("two failures no break") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "x"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    check(!agent_loop_should_break(msgs).has_value(),
          "2x failing call does NOT break (1 retry allowed)");
}

// ── 3. Same call but SUCCEEDING → never break (legit re-read) ────────────────
TEST_CASE("repeat succeeding tolerated below the limit") {
    // A repeat that WORKS is weak evidence of a stuck model: re-reading a
    // file after editing it, or re-running a test, is ordinary. So the
    // threshold sits above the failure one and five is still fine.
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "log.txt"}};
    for (int i = 0; i < 5; ++i)
        msgs.push_back(asst_call("read", a, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "five identical successful calls are still legitimate");
}

TEST_CASE("repeat succeeding breaks at the limit") {
    // The gap this closes. The failure breaker only counted FAILING calls,
    // and the step cap that used to back it up was gated on a model-id
    // heuristic (and has since been removed entirely). A model behind a
    // private gateway (`igpu/laguna-xs-2.1`) is unrecognised by
    // construction, so an identical-call loop that SUCCEEDED every time had
    // no cap at all: measured at 80 repeats before the turn budget ran out,
    // which on a 22GB resident is minutes of GPU time going nowhere.
    //
    // Applies to EVERY model — and now that the step cap is gone, this IS
    // the backstop for a loop that keeps succeeding. It keys on the call
    // being IDENTICAL, which is evidence of being stuck rather than evidence
    // of working hard.
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "log.txt"}};
    for (int i = 0; i < 6; ++i)
        msgs.push_back(asst_call("read", a, Term::Done));
    auto brk = agent_loop_should_break(msgs);
    check(brk.has_value(), "six identical successful calls break the loop");
    if (brk) check(brk->reason.find("read") != std::string::npos,
                   "the break names the offending tool");
}

TEST_CASE("succeeding repeats with different args never break") {
    // The discriminator: it is the IDENTICAL call that is evidence of a
    // loop, not the tool. A model walking a directory reads many files.
    std::vector<Message> msgs;
    msgs.push_back(user());
    for (int i = 0; i < 12; ++i)
        msgs.push_back(asst_call("read",
            json{{"path", "f" + std::to_string(i) + ".txt"}}, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "distinct successful reads are a legitimate traversal");
}

// ── 4. Different args each time → not the same dead call ─────────────────────
TEST_CASE("distinct failing calls no break") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    // 3 failing reads but each a DIFFERENT path — exploring, not stuck.
    msgs.push_back(asst_call("read", json{{"path", "a"}}, Term::Failed));
    msgs.push_back(asst_call("read", json{{"path", "b"}}, Term::Failed));
    msgs.push_back(asst_call("read", json{{"path", "c"}}, Term::Failed));
    check(!agent_loop_should_break(msgs).has_value(),
          "distinct failing args do NOT trip the repeat cap");
}

// ── 5. NO STEP CAP: a long run of DISTINCT work is never cut off ─────────────
//
// There used to be a 25-turn cap here, gated on a model-id heuristic. Both are
// gone: a raw count of tool turns is not evidence of anything. A real
// search → read → edit → verify pass over a large codebase legitimately spends
// dozens of distinct, productive steps, and stopping it at an arbitrary number
// truncates working output with no finding to point at. The gate made it worse
// by being inconsistent — the SAME transcript stopped on a local model and ran
// fine on Claude.
//
// What still bounds a runaway is repetition, which is tested in 5c/5d below.
// This case locks in that volume alone never trips anything.
TEST_CASE("no step cap: distinct work runs as long as it needs") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    for (int i = 0; i < 200; ++i)
        msgs.push_back(asst_call("shell", json{{"command", "echo " + std::to_string(i)}},
                                 Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "200 distinct successful tool turns do NOT break");
}

// ── 5b. ...and that holds regardless of WHICH model is driving.
//       The old cap was enforced only when an id heuristic failed to
//       recognise the model, so an unfamiliar local name got stopped where
//       `claude-opus-4` did not, on byte-identical history. There is no flag
//       left to pass, which is the point: one transcript, one answer.
TEST_CASE("no step cap: the answer does not depend on the model id") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    for (int i = 0; i < 60; ++i)
        msgs.push_back(asst_call("read", json{{"path", "f" + std::to_string(i) + ".cpp"}},
                                 Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "60 distinct reads never break, whoever is driving");
}

// ── 5c. REPEAT-FAILURE is UNIVERSAL — and with the step cap gone it is one
//       of only two things that can stop a run at all. aider/MindStudio apply
//       the repeated-dead-call cap to every model; a capable model stuck
//       re-trying an identical failing call still stops.
TEST_CASE("repeat failure breaks even for capable") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "/nope"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    auto brk = agent_loop_should_break(msgs);
    check(brk.has_value(),
          "3x identical failing call breaks on every model");
    check(brk && brk->reason.find("read") != std::string::npos,
          "repeat-failure reason names the tool");
}

// ── 5d. Only the CURRENT consecutive streak counts. Earlier failures that
//       were followed by success are recovery history, not a doom loop.
TEST_CASE("success resets failure streak") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "flaky.txt"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Done));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    check(!agent_loop_should_break(msgs).has_value(),
          "success resets identical-call failure streak");
}

// Rejections are terminal failures for loop purposes: repeatedly refusing the
// same permission cannot make progress and should receive the same breaker.
TEST_CASE("rejection streak breaks") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"command", "dangerous"}};
    msgs.push_back(asst_call("shell", a, Term::Rejected));
    msgs.push_back(asst_call("shell", a, Term::Rejected));
    msgs.push_back(asst_call("shell", a, Term::Rejected));
    check(agent_loop_should_break(msgs).has_value(),
          "3x identical rejection streak breaks");
}

// ── 6. A modest healthy multi-step task does NOT break ───────────────────────
TEST_CASE("healthy progress no break") {
    std::vector<Message> msgs;
    msgs.push_back(user());
    msgs.push_back(asst_call("shell",  json{{"command", "ls"}}, Term::Done));
    msgs.push_back(asst_call("read",  json{{"path", "a.cpp"}}, Term::Done));
    msgs.push_back(asst_call("edit",  json{{"path", "a.cpp"}}, Term::Done));
    msgs.push_back(asst_call("shell",  json{{"command", "make"}}, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "4-step search→read→edit→verify does NOT break");
}

// ── 7. User boundary resets the run: a prior doom loop in an EARLIER turn
//      doesn't count against the current one. ──────────────────────────────
TEST_CASE("user boundary resets run") {
    std::vector<Message> msgs;
    // Earlier turn: a doom loop (3 failing reads).
    msgs.push_back(user("first"));
    json a = {{"path", "x"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    // New user turn starts a FRESH run with one healthy call.
    msgs.push_back(user("second"));
    msgs.push_back(asst_call("shell", json{{"command", "pwd"}}, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "doom loop in a PRIOR run doesn't break the current fresh run");
}

// ── 8. Empty / no-tool history is safe ───────────────────────────────────────
TEST_CASE("empty and text only no break") {
    check(!agent_loop_should_break({}).has_value(), "empty history no break");
    std::vector<Message> msgs;
    msgs.push_back(user());
    msgs.push_back(asst_text("Here's your answer."));
    check(!agent_loop_should_break(msgs).has_value(), "text-only turn no break");
}

