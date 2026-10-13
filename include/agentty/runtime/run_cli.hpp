#pragma once
// agentty run: options parsed by main(), and the entry point.

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <maya/runtime.hpp>

namespace agentty::run_cli {

struct Options {
    std::string prompt;
    std::string agent;                  // empty: general
    std::string output;                 // -o FILE; empty: stdout
    int  max_turns = 0;                 // 0: the role's default
    std::chrono::seconds timeout{0};    // 0: the default wall clock
    bool json = false;
    bool verbose = false;               // one stats line on stderr
    bool read_only = false;             // --read-only
    bool no_network = false;            // --no-network
    std::string effort;                 // --effort
    std::string resume;                 // --resume ID|last
    bool save = false;                  // --save
    bool events = false;                // --events jsonl (reasoning/text too)
};

// Runs the prompt and returns the process exit code:
//   0 ok, 1 task failed or out of turns, 3 provider/auth problem,
//   4 --resume thread not found, 5 context window full,
//   124 timed out, 130/143/129 stopped by SIGINT/SIGTERM/SIGHUP.
// Runs the agent and waits for it (and its signal watcher): `unlocked`.
[[nodiscard]] int run(const maya::unlocked&, const Options& o);

// "25" -> 25 (1..500). nullopt when it isn't one.
[[nodiscard]] std::optional<int> parse_count(std::string_view s);
// "90", "90s", "5m", "1h" -> seconds (> 0). nullopt when it isn't one.
[[nodiscard]] std::optional<std::chrono::seconds> parse_duration(std::string_view s);
// A thread id (16 hex digits) or "last".
[[nodiscard]] bool valid_resume(std::string_view s);
// low|medium|high|xhigh|max|auto.
[[nodiscard]] bool valid_effort(std::string_view s);

}  // namespace agentty::run_cli
