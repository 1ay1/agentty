#pragma once
// agentty run: one prompt through the subagent loop, no UI.
// Kept free of mcp headers so main.cpp can include it.

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

namespace agentty::tools {

struct OneShotOptions {
    std::string prompt;
    std::string agent_type;              // empty: general
    int max_turns = 0;                   // 0: the role's default
    std::chrono::seconds timeout{0};     // 0: the default wall clock
    std::stop_token cancel;              // stops the run (Ctrl-C, SIGTERM)
};

struct OneShotResult {
    enum class Status { Ok, Failed, TurnLimit, Timeout, ProviderError, Cancelled, Unavailable };
    Status      status = Status::Ok;
    std::string text;       // the model's last answer; may be partial on failure
    std::string error;      // why it didn't succeed; empty when Ok
    std::string agent;
    std::string model;
    int         turns = 0;
    int         tool_calls = 0;
    int         tool_errors = 0;
    long long   input_tokens = 0;    // summed over requests
    long long   output_tokens = 0;
    long long   duration_ms = 0;

    [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
};

[[nodiscard]] OneShotResult run_one_shot(const OneShotOptions& opts);

// "ok", "failed", "turn_limit", ... for --json and messages.
[[nodiscard]] std::string_view status_name(OneShotResult::Status s) noexcept;

}  // namespace agentty::tools
