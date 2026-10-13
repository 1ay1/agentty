#pragma once
// agentty run: one prompt through the subagent loop, no UI.
// Kept free of mcp headers so main.cpp can include it.

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

#include <maya/runtime.hpp>

namespace agentty::tools {

struct OneShotOptions {
    std::string prompt;
    std::string agent_type;              // empty: general
    int max_turns = 0;                   // 0: the role's default
    std::chrono::seconds timeout{0};     // 0: the default wall clock
    std::stop_token cancel;              // stops the run (Ctrl-C, SIGTERM)
    bool read_only = false;              // no write/exec/net tools
    bool no_network = false;             // no net tools (shell net is the sandbox's job)
    std::string effort;                  // "": off; else low|medium|high|xhigh|max|auto
    bool events = false;                 // reasoning/text/turn lines on stderr
    std::string resume;                  // thread id to continue; "" for a new thread
    bool save = false;                   // save as a thread (always when resuming)
};

struct OneShotResult {
    enum class Status { Ok, Failed, TurnLimit, Timeout, ProviderError, Cancelled, Unavailable,
                        NotFound, ContextFull };
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
    long long   last_input_tokens = 0;  // prompt size of the last request
    long long   context_window = 0;     // the model's window, 0 if unknown
    long long   duration_ms = 0;
    std::string thread_id;   // where the conversation was saved; "" if not

    [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
};

[[nodiscard]] OneShotResult run_one_shot(const maya::unlocked&, const OneShotOptions& opts);

// "ok", "failed", "turn_limit", ... for --json and messages.
[[nodiscard]] std::string_view status_name(OneShotResult::Status s) noexcept;

}  // namespace agentty::tools
