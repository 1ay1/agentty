// speculative_dispatch_args_test — a read-only tool is only dispatched early
// when its arguments actually arrived.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// StreamToolUseEnd dispatches read-only, permission-free tools immediately
// instead of waiting for finalize_turn: a `read` can start while the model is
// still typing the next sentence. The gate checked "args parsed clean" as
// `!args.is_null()`, which a SALVAGED arg set also satisfies.
//
// salvage_args only refuses a cut that landed inside a string value. A cut
// that lands outside one — right after `"symbol": "dispatch_stream",` —
// parses into a tidy object that happens to be missing `path`. The normal
// path handles that: guard_truncated_tool_args notices the missing
// schema-required field, repairs what it can, and finalize_turn retries the
// stream transparently. But guard_truncated_tool_args only ever sees a tool
// that is still Pending, so dispatching here skipped the entire mechanism —
// for exactly the read-only tools this path exists to accelerate.
//
// Mined from 40 recent threads: 44 of 3323 `read` calls died as
// "[invalid args] path required", 33 of them with args `{}`. To the model
// that reads as its own mistake, so it re-emits the same call; the real
// cause was the wire cutting mid-arguments.
//
// Pinned here: incomplete args leave the tool Pending for finalize_turn to
// own, and complete args still get the fast path.
#include <chrono>
#include <string>
#include <utility>

#include "agtest.hpp"

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace A = agentty;
namespace D = agentty::app::detail;

namespace {

A::Model apply(A::Model m, A::msg::StreamMsg event) {
    auto [next, cmd] = D::step(::agentty::app::detail::stream_update,
                               std::move(m), std::move(event));
    (void)cmd;
    return std::move(next);
}

// A model mid-stream, in the profile where a read needs no permission —
// the configuration the speculative path is built for.
A::Model streaming_model() {
    A::Model m;
    m.d.profile = A::Profile::Write;
    A::Message assistant;
    assistant.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(assistant));
    A::phase::Active active;
    active.last_event_at = std::chrono::steady_clock::now();
    m.s.phase = A::phase::Streaming{std::move(active)};
    return m;
}

// Stream one `read` call whose argument bytes are exactly `json`.
A::Model stream_read_call(std::string json) {
    auto m = streaming_model();
    const A::ToolCallId id{"tc_spec_1"};
    m = apply(std::move(m), A::StreamToolUseStart{id, A::ToolName{"read"}});
    m = apply(std::move(m), A::StreamToolUseDelta{id, std::move(json)});
    m = apply(std::move(m), A::StreamToolUseEnd{id});
    return m;
}

const A::ToolUse* only_call(const A::Model& m) {
    for (const auto& msg : m.d.current.messages)
        for (const auto& tc : msg.tool_calls)
            if (tc.id.value == "tc_spec_1") return &tc;
    return nullptr;
}

} // namespace

TEST_CASE("speculative dispatch: complete args still take the fast path") {
    auto m = stream_read_call(R"({"path":"src/main.cpp","limit":40})");
    const auto* tc = only_call(m);
    REQUIRE(tc != nullptr);
    CHECK_MESSAGE(tc->is_running(),
                  "a clean read must still start while the model keeps typing");
    CHECK(tc->args.value("path", std::string{}) == "src/main.cpp");
}

TEST_CASE("speculative dispatch: args cut outside a string stay Pending") {
    // The exact shape from the threads: display_description and symbol
    // arrived, `path` did not, and the cut is outside a string so the
    // salvage closes the object happily.
    auto m = stream_read_call(
        R"({"display_description":"Read dispatch_stream","symbol":"dispatch_stream",)");
    const auto* tc = only_call(m);
    REQUIRE(tc != nullptr);
    CHECK_MESSAGE(!tc->is_running(),
                  "a read with no path can only fail -- dispatching it burns "
                  "the turn and blames the model");
    CHECK_MESSAGE(tc->is_pending(),
                  "left Pending so finalize_turn can repair, retry, then fail "
                  "with an honest message");
}

TEST_CASE("speculative dispatch: empty args stay Pending") {
    // 33 of the 44 mined failures had args `{}` — nothing arrived at all.
    auto m = stream_read_call("");
    const auto* tc = only_call(m);
    REQUIRE(tc != nullptr);
    CHECK_FALSE(tc->is_running());
}

TEST_CASE("speculative dispatch: a tool with no required args is unaffected") {
    // git_status takes nothing, so `{}` is complete for it and the fast path
    // must not be withheld.
    auto m = streaming_model();
    const A::ToolCallId id{"tc_spec_1"};
    m = apply(std::move(m), A::StreamToolUseStart{id, A::ToolName{"git_status"}});
    m = apply(std::move(m), A::StreamToolUseDelta{id, "{}"});
    m = apply(std::move(m), A::StreamToolUseEnd{id});

    const auto* tc = only_call(m);
    REQUIRE(tc != nullptr);
    CHECK_MESSAGE(tc->is_running(),
                  "an argumentless read-only tool keeps the fast path");
}
