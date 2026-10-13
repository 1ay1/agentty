// cache_churn_bench — how much of the previous request's prefix does each new
// request invalidate?
//
// Replays a REAL saved thread one request at a time (every prefix ending in
// an assistant turn with tool calls = one agent-loop request), builds the
// exact Anthropic `messages` body for each, and compares consecutive bodies.
// The prompt cache can only reuse bytes up to the first difference, so
//   invalidated = len(prev) - common_prefix(prev, next)
// is prefix the API re-bills as cache_creation instead of cache_read. Ideal
// is ~0 on every step (pure append). Reports the total and the worst steps
// with the message index where the divergence starts.
//   agentty_standalone_tests cache_churn_bench <thread.jsonl>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agentty/io/thread_log.hpp"
#include "agentty/provider/anthropic/transport.hpp"

using namespace agentty;

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: cache_churn_bench <thread.jsonl>\n"); return 2; }
    auto log = ThreadLog::open_path(argv[1]);
    if (!log) { std::printf("could not open %s\n", argv[1]); return 1; }
    const Thread full = log->load_thread();

    std::string prev;
    std::size_t total_prev = 0, total_lost = 0, steps = 0, broke = 0;
    struct Worst { std::size_t lost; std::size_t at_msg; std::string ctx; };
    std::vector<Worst> worst;
    for (std::size_t n = 2; n <= full.messages.size(); ++n) {
        const auto& last = full.messages[n - 1];
        if (last.role != Role::Assistant || last.tool_calls.empty()) continue;
        Thread t = full;
        t.messages.resize(n);
        std::string body = provider::anthropic::messages_json_string(t);
        // Strip cache_control markers: they are EXPECTED to move each turn
        // and are not part of the cached content.
        for (std::size_t p; (p = body.find(",\"cache_control\":{")) != std::string::npos;) {
            auto e = body.find('}', p);
            body.erase(p, e - p + 1);
        }
        if (!prev.empty()) {
            const auto c = static_cast<std::size_t>(
                std::mismatch(prev.begin(), prev.end(), body.begin(), body.end()).first
                - prev.begin());
            const std::size_t lost = prev.size() - c;
            ++steps; total_prev += prev.size(); total_lost += lost;
            if (lost > 64) {
                ++broke;
                // Which wire message does the divergence fall in?
                std::size_t msg = 0, depth = 0;
                bool in_str = false, esc = false;
                for (std::size_t i = 0; i < c; ++i) {
                    char ch = prev[i];
                    if (in_str) { if (esc) esc = false; else if (ch == '\\') esc = true; else if (ch == '"') in_str = false; continue; }
                    if (ch == '"') in_str = true;
                    else if (ch == '{' || ch == '[') { if (++depth == 2 && ch == '{') ++msg; }
                    else if (ch == '}' || ch == ']') --depth;
                }
                worst.push_back({lost, msg, prev.substr(c > 60 ? c - 60 : 0, 140)});
            }
        }
        prev = std::move(body);
    }
    std::sort(worst.begin(), worst.end(), [](auto& a, auto& b) { return a.lost > b.lost; });
    std::printf("thread %s: %zu messages, %zu agent-loop requests\n",
                argv[1], full.messages.size(), steps + 1);
    std::printf("steps that rewrote history: %zu of %zu\n", broke, steps);
    std::printf("prefix invalidated: %.1f KB of %.1f KB replayed (%.2f%%)\n",
                total_lost / 1024.0, total_prev / 1024.0,
                total_prev ? 100.0 * total_lost / total_prev : 0.0);
    for (std::size_t i = 0; i < std::min<std::size_t>(worst.size(), 8); ++i) {
        std::string ctx = worst[i].ctx;
        for (auto& ch : ctx) if (ch == '\n') ch = ' ';
        std::printf("  lost %7zu B at wire msg #%-4zu  ...%s...\n",
                    worst[i].lost, worst[i].at_msg, ctx.c_str());
    }
    return 0;
}
