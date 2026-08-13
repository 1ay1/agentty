// smart_cascade_gate_test — the finalize_turn cascade must fire ONCE per user
// turn, at the FINAL settle, not on every intermediate tool round-trip.
//
// finalize_turn runs on each StreamFinished. A tool-using turn produces several
// StopReason::ToolUse settles (one per round-trip) before its real EndTurn.
// Before the gate, the whole cascade block (bias decay, note_regret,
// DecompositionMemory::record) ran on each of those — decaying the bias N
// times, poisoning the RoutingMemory denominator, and recording the same
// decomposition repeatedly. This test drives finalize_turn through a simulated
// multi-round-trip turn and asserts the cascade side-effects happen exactly
// once, on the EndTurn settle.
//
// Observable: smart::DecompositionMemory::learned_count() (Innovation 4 records
// a Complex turn's decomposition exactly once) against an isolated temp root.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "agentty/domain/decomposition_memory.hpp"
#include "agentty/domain/routing_memory.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"

namespace provider = agentty::provider;
namespace store    = agentty::store;
using agentty::ThreadId;

namespace fs = std::filesystem;
using agentty::Message;
using agentty::Model;
using agentty::Role;
using agentty::StopReason;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;
using nlohmann::json;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

// A `task` delegation call in a given terminal state.
static ToolUse task_call(const std::string& agent_type, bool done) {
    ToolUse tc;
    tc.id   = ToolCallId{"toolu_" + agent_type};
    tc.name = ToolName{"task"};
    tc.args = json{{"agent_type", agent_type}, {"prompt", "map the " + agent_type + " area"}};
    if (done)
        tc.status = ToolUse::Done{{}, {}, "report"};
    else
        tc.status = ToolUse::Pending{};
    return tc;
}

// A Model mid-turn: a Complex user turn that spawned two parallel explorers,
// across two assistant messages (the shape a real orchestrated turn takes).
static Model make_turn(bool tools_done) {
    Model m;
    m.d.smart.enabled   = true;
    m.d.smart.orchestrate   = true;
    m.d.smart.learn_routing = true;
    m.d.smart.recall_plans  = true;
    m.d.smart.outcome_feedback = true;

    Message user;
    user.role = Role::User;
    user.text = "investigate why startup is slow and fix it end to end";
    m.d.current.messages.push_back(std::move(user));

    Message a1;
    a1.role = Role::Assistant;
    a1.text = "delegating";
    a1.tool_calls.push_back(task_call("explorer", tools_done));
    m.d.current.messages.push_back(std::move(a1));

    Message a2;
    a2.role = Role::Assistant;
    a2.text = "delegating more";
    a2.tool_calls.push_back(task_call("reviewer", tools_done));
    m.d.current.messages.push_back(std::move(a2));

    // The launcher would have stashed these at launch_stream.
    m.s.smart_turn_complexity = agentty::smart::Complexity::Complex;
    m.s.smart_turn_signature  =
        agentty::smart::turn_signature(agentty::smart::Complexity::Complex, user.text);
    return m;
}

int main() {
    // Isolate both stores in a temp workspace.
    auto root = fs::temp_directory_path() /
                ("agentty_cascade_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    agentty::smart::DecompositionMemory::instance().set_root_for_test(root.string());
    agentty::smart::RoutingMemory::instance().set_root_for_test(root.string());

    // finalize_turn reaches the Store seam (persistence) past the cascade;
    // install inert stubs so the reducer runs without a real backend.
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"stub"}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });

    // ── 1. Mid-turn settle (StopReason::ToolUse): tools still pending, so the
    //       cascade must be SKIPPED — no decomposition recorded, no decay.
    {
        Model m = make_turn(/*tools_done=*/false);
        const int bias_before = m.s.smart_effort_bias;
        (void)agentty::app::detail::finalize_turn(m, StopReason::ToolUse);
        CHECK(agentty::smart::DecompositionMemory::instance().learned_count() == 0,
              "mid-turn (ToolUse) settle records NO decomposition");
        CHECK(m.s.smart_effort_bias == bias_before,
              "mid-turn settle does not decay/adjust the session bias");
    }

    // ── 2. Also mid-turn if the model stopped EndTurn but a tool call is still
    //       pending/approved (kick_pending_tools would promote it): SKIP.
    {
        Model m = make_turn(/*tools_done=*/false);   // pending calls
        (void)agentty::app::detail::finalize_turn(m, StopReason::EndTurn);
        CHECK(agentty::smart::DecompositionMemory::instance().learned_count() == 0,
              "pending tool calls ⇒ cascade skipped even on EndTurn");
    }

    // ── 3. Final settle (EndTurn, all tools Done): cascade fires ONCE. A
    //       Complex turn with ≥1 delegation records its decomposition exactly
    //       once, however many round-trips preceded it.
    {
        Model m = make_turn(/*tools_done=*/true);
        (void)agentty::app::detail::finalize_turn(m, StopReason::EndTurn);
        const auto after_first =
            agentty::smart::DecompositionMemory::instance().learned_count();
        CHECK(after_first == 1, "final settle records the decomposition exactly once");

        // Re-running the SAME settled turn must not double-record (dedup on
        // identical steps) — the record is idempotent for a stable turn.
        (void)agentty::app::detail::finalize_turn(m, StopReason::EndTurn);
        CHECK(agentty::smart::DecompositionMemory::instance().learned_count() == after_first,
              "re-settling the same turn does not duplicate the decomposition");
    }

    fs::remove_all(root);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
