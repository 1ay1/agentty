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

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

enum class Term { Done, Failed };

ToolUse call(const std::string& name, json args, Term t) {
    static int seq = 0;
    ToolUse tc;
    tc.id   = ToolCallId{"call_" + std::to_string(seq++)};
    tc.name = ToolName{name};
    tc.args = std::move(args);
    if (t == Term::Done) tc.status = ToolUse::Done{{}, {}, "ok"};
    else                 tc.status = ToolUse::Failed{{}, {}, "no such file"};
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

// ── 1. REPEAT: same failing call 3x → break ─────────────────────────────────
void test_repeat_failing_call_breaks() {
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
void test_two_failures_no_break() {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "x"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    check(!agent_loop_should_break(msgs).has_value(),
          "2x failing call does NOT break (1 retry allowed)");
}

// ── 3. Same call but SUCCEEDING → never break (legit re-read) ────────────────
void test_repeat_succeeding_no_break() {
    std::vector<Message> msgs;
    msgs.push_back(user());
    json a = {{"path", "log.txt"}};
    for (int i = 0; i < 5; ++i)
        msgs.push_back(asst_call("read", a, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "repeated SUCCEEDING call never breaks");
}

// ── 4. Different args each time → not the same dead call ─────────────────────
void test_distinct_failing_calls_no_break() {
    std::vector<Message> msgs;
    msgs.push_back(user());
    // 3 failing reads but each a DIFFERENT path — exploring, not stuck.
    msgs.push_back(asst_call("read", json{{"path", "a"}}, Term::Failed));
    msgs.push_back(asst_call("read", json{{"path", "b"}}, Term::Failed));
    msgs.push_back(asst_call("read", json{{"path", "c"}}, Term::Failed));
    check(!agent_loop_should_break(msgs).has_value(),
          "distinct failing args do NOT trip the repeat cap");
}

// ── 5. RUNAWAY: 25 healthy tool turns → break on step cap ────────────────────
void test_runaway_step_cap_breaks() {
    std::vector<Message> msgs;
    msgs.push_back(user());
    for (int i = 0; i < 25; ++i)
        msgs.push_back(asst_call("bash", json{{"command", "echo " + std::to_string(i)}},
                                 Term::Done));
    auto brk = agent_loop_should_break(msgs);
    check(brk.has_value(), "25 tool turns → runaway break");
    check(brk && brk->reason.find("steps") != std::string::npos,
          "runaway reason mentions step count");
}

// ── 6. A modest healthy multi-step task does NOT break ───────────────────────
void test_healthy_progress_no_break() {
    std::vector<Message> msgs;
    msgs.push_back(user());
    msgs.push_back(asst_call("bash",  json{{"command", "ls"}}, Term::Done));
    msgs.push_back(asst_call("read",  json{{"path", "a.cpp"}}, Term::Done));
    msgs.push_back(asst_call("edit",  json{{"path", "a.cpp"}}, Term::Done));
    msgs.push_back(asst_call("bash",  json{{"command", "make"}}, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "4-step search→read→edit→verify does NOT break");
}

// ── 7. User boundary resets the run: a prior doom loop in an EARLIER turn
//      doesn't count against the current one. ──────────────────────────────
void test_user_boundary_resets_run() {
    std::vector<Message> msgs;
    // Earlier turn: a doom loop (3 failing reads).
    msgs.push_back(user("first"));
    json a = {{"path", "x"}};
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    msgs.push_back(asst_call("read", a, Term::Failed));
    // New user turn starts a FRESH run with one healthy call.
    msgs.push_back(user("second"));
    msgs.push_back(asst_call("bash", json{{"command", "pwd"}}, Term::Done));
    check(!agent_loop_should_break(msgs).has_value(),
          "doom loop in a PRIOR run doesn't break the current fresh run");
}

// ── 8. Empty / no-tool history is safe ───────────────────────────────────────
void test_empty_and_text_only_no_break() {
    check(!agent_loop_should_break({}).has_value(), "empty history no break");
    std::vector<Message> msgs;
    msgs.push_back(user());
    msgs.push_back(asst_text("Here's your answer."));
    check(!agent_loop_should_break(msgs).has_value(), "text-only turn no break");
}

} // namespace

int main() {
    test_repeat_failing_call_breaks();
    test_two_failures_no_break();
    test_repeat_succeeding_no_break();
    test_distinct_failing_calls_no_break();
    test_runaway_step_cap_breaks();
    test_healthy_progress_no_break();
    test_user_boundary_resets_run();
    test_empty_and_text_only_no_break();

    if (g_fails == 0) {
        std::printf("doom_loop_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "doom_loop_test: %d check(s) failed\n", g_fails);
    return 1;
}
