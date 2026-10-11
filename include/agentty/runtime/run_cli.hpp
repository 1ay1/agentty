#pragma once
// agentty run: options parsed by main(), and the entry point.

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace agentty::run_cli {

struct Options {
    std::string prompt;
    std::string agent;                  // empty: general
    std::string output;                 // -o FILE; empty: stdout
    int  max_turns = 0;                 // 0: the role's default
    std::chrono::seconds timeout{0};    // 0: the default wall clock
    bool json = false;
    bool verbose = false;               // one stats line on stderr
};

// Runs the prompt and returns the process exit code:
//   0 ok, 1 task failed or out of turns, 3 provider/auth problem,
//   124 timed out, 130/143/129 stopped by SIGINT/SIGTERM/SIGHUP.
[[nodiscard]] int run(const Options& o);

// "25" -> 25 (1..500). nullopt when it isn't one.
[[nodiscard]] std::optional<int> parse_count(std::string_view s);
// "90", "90s", "5m", "1h" -> seconds (> 0). nullopt when it isn't one.
[[nodiscard]] std::optional<std::chrono::seconds> parse_duration(std::string_view s);

}  // namespace agentty::run_cli
