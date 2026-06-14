// salvage_dedup_test — re-leaked salvaged-tool-call dedup.
//
// Weak local models on the OpenAI-compat path (qwen2.5-coder:7b etc.) leak
// tool calls as bare JSON in `content`; the transport salvages them with a
// synthetic `call_salvaged_N` id. These models then RE-LEAK the identical call
// on the post-tool sub-turn, so without a guard it runs twice (the duplicate
// stuck card bug). dedup_releaked_salvage_calls resolves a pending salvaged
// call as Failed-without-side-effects when an identical call already ran
// terminal earlier in the same agent turn.
//
// Asserts:
//   1. A re-leaked salvaged call (same name+args, prior Done) is deduped →
//      Failed, never executed.
//   2. A STRUCTURED duplicate (real `call_...`/`toolu_...` id) is NOT deduped
//      — calling read twice with same args is legitimate intent.
//   3. A salvaged call with DIFFERENT args is NOT deduped.
//   4. The FIRST salvaged call (no prior terminal twin) is left Pending.
//   5. Dedup scope is the current turn — a User boundary resets it.

#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"

using agentty::Message;
using agentty::Model;
using agentty::Role;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using nlohmann::json;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

ToolUse make_call(const std::string& id, const std::string& name,
                  json args, bool terminal) {
    ToolUse tc;
    tc.id   = ToolCallId{id};
    tc.name = ToolName{name};
    tc.args = std::move(args);
    if (terminal)
        tc.status = ToolUse::Done{ {}, {}, "ok" };
    else
        tc.status = ToolUse::Pending{ {} };
    return tc;
}

Message asst(std::vector<ToolUse> calls) {
    Message m;
    m.role = Role::Assistant;
    for (auto& c : calls) m.tool_calls.push_back(std::move(c));
    return m;
}

Message user() {
    Message m;
    m.role = Role::User;
    m.text = "go";
    return m;
}

// ── 1. Re-leaked salvaged call (same name+args) is deduped ──────────────────
void test_releak_same_turn_deduped() {
    Model m;
    // One assistant message: a prior Done salvaged remember + a re-leaked
    // Pending salvaged remember with identical args.
    json a = {{"text", "Hi there!"}};
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_0", "remember", a, /*terminal=*/true),
        make_call("call_salvaged_1", "remember", a, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 1, "exactly one call deduped");
    const auto& tcs = m.d.current.messages.back().tool_calls;
    check(tcs[0].is_done(),   "prior call stays Done");
    check(tcs[1].is_failed(), "re-leaked call resolved Failed (not run)");
}

// ── 2. Structured duplicate is NOT deduped (deliberate intent) ──────────────
void test_structured_duplicate_not_deduped() {
    Model m;
    json a = {{"path", "/tmp/x"}};
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_abc", "read", a, /*terminal=*/true),
        make_call("call_def", "read", a, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 0, "structured duplicate left alone");
    check(m.d.current.messages.back().tool_calls[1].is_pending(),
          "structured duplicate stays Pending");
}

// ── 3. Salvaged call with DIFFERENT args is NOT deduped ─────────────────────
void test_salvaged_different_args_not_deduped() {
    Model m;
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_0", "remember",
                  json{{"text", "fact A"}}, /*terminal=*/true),
        make_call("call_salvaged_1", "remember",
                  json{{"text", "fact B"}}, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 0, "different-args salvaged call not deduped");
    check(m.d.current.messages.back().tool_calls[1].is_pending(),
          "different-args call stays Pending");
}

// ── 4. First salvaged call (no prior twin) is left Pending ──────────────────
void test_first_salvaged_call_runs() {
    Model m;
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_0", "remember",
                  json{{"text", "x"}}, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 0, "first salvaged call not deduped");
    check(m.d.current.messages.back().tool_calls[0].is_pending(),
          "first salvaged call stays Pending");
}

// ── 5. Dedup is scoped to the current turn (User boundary resets) ───────────
void test_prior_turn_not_counted() {
    Model m;
    json a = {{"text", "x"}};
    // Turn 1: salvaged remember ran Done.
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_0", "remember", a, /*terminal=*/true),
    }));
    // Turn 2: a NEW user message, then the same call again pending. This is a
    // fresh deliberate request — must NOT be deduped against turn 1.
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_1", "remember", a, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 0, "duplicate in a NEW turn not deduped against prior turn");
    check(m.d.current.messages.back().tool_calls[0].is_pending(),
          "new-turn call stays Pending");
}

// ── 6. Cross-sub-turn re-leak (the actual bug shape) is deduped ─────────────
// The real loop: assistant sub-turn A runs the salvaged call Done; a post-tool
// sub-turn B (no intervening User) re-leaks it Pending. Both are Assistant
// messages in the same turn.
void test_cross_subturn_releak_deduped() {
    Model m;
    json a = {{"text", "Hi there!"}};
    m.d.current.messages.push_back(user());
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_0", "remember", a, /*terminal=*/true),
    }));
    // Sub-turn B (continuation placeholder that streamed the re-leak).
    m.d.current.messages.push_back(asst({
        make_call("call_salvaged_1", "remember", a, /*terminal=*/false),
    }));

    auto n = agentty::app::cmd::dedup_releaked_salvage_calls(m);
    check(n == 1, "cross-sub-turn re-leak deduped");
    check(m.d.current.messages.back().tool_calls[0].is_failed(),
          "cross-sub-turn re-leak resolved Failed");
}

} // namespace

int main() {
    test_releak_same_turn_deduped();
    test_structured_duplicate_not_deduped();
    test_salvaged_different_args_not_deduped();
    test_first_salvaged_call_runs();
    test_prior_turn_not_counted();
    test_cross_subturn_releak_deduped();

    if (g_fails == 0) {
        std::printf("salvage_dedup_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "salvage_dedup_test: %d check(s) failed\n", g_fails);
    return 1;
}
