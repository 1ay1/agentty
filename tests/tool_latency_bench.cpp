// tool_latency_bench — wall-clock latency of the hot built-in tools.
//
// Drives each tool through the real registry (the same ToolDef::execute the
// agent loop calls) against a real source tree, N runs each, and prints
// p50 / p90 / max in microseconds. Not a pass/fail test: a measuring stick
// for "is the tool layer fast?". Point it at a tree with
//   agentty_standalone_tests tool_latency_bench [workspace] [runs]
// (default: the current directory, 30 runs).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace agentty;

namespace {

struct Case { const char* label; const char* tool; json args; };

long pct(std::vector<long> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * v.size()))];
}

} // namespace

int main(int argc, char** argv) {
    const fs::path root = fs::canonical(argc > 1 ? argv[1] : ".");
    const int runs = argc > 2 ? std::atoi(argv[2]) : 30;
    fs::current_path(root);   // the agent runs with cwd = workspace
    ::setenv("AGENTTY_OLLAMA_HOST", "127.0.0.1:1", 1);
    ::unsetenv("AGENTTY_MCP_CONFIG");
    tools::util::set_workspace_root(root);
    tools::wire_mcp_runtime(std::getenv("BENCH_SANDBOX") ? "auto" : "off");
    (void)tools::registry();

    // Pick a real file in the tree for read/outline-style cases.
    std::string some_file = "README.md";
    if (!fs::exists(root / some_file)) {
        for (auto& e : fs::recursive_directory_iterator(root))
            if (e.is_regular_file() && e.path().extension() == ".cpp") {
                some_file = fs::relative(e.path(), root).string(); break;
            }
    }

    const std::vector<Case> cases = {
        {"shell echo",        "shell",    {{"command", "echo hi"}}},
        {"shell sed",         "shell",    {{"command", "sed -n 1,20p " + some_file}}},
        {"read 200 lines",    "read",     {{"path", some_file}, {"limit", 200}}},
        {"grep literal",      "grep",     {{"pattern", "namespace"}, {"output", "count"}}},
        {"grep regex+ctx",    "grep",     {{"pattern", "std::(vector|string)<"}, {"context", "0"}}},
        {"glob *.cpp",        "glob",     {{"pattern", "*.cpp"}}},
        {"list_dir",          "list_dir", {{"path", "."}}},
        {"find_definition",   "find_definition", {{"symbol", "main"}}},
        {"outline",           "outline",  {{"path", some_file}}},
        {"git_status",        "git_status", json::object()},
    };

    std::printf("tool_latency_bench  root=%s  runs=%d\n", root.c_str(), runs);
    std::printf("%-18s %9s %9s %9s  %s\n", "case", "p50 us", "p90 us", "max us", "ok");
    for (const auto& c : cases) {
        const auto* td = tools::find(c.tool);
        if (!td) { std::printf("%-18s  (tool not registered)\n", c.label); continue; }
        std::vector<long> us;
        bool ok = true;
        (void)td->execute(c.args);   // warm: first-touch caches, page cache
        for (int i = 0; i < runs; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            auto r = td->execute(c.args);
            const auto t1 = std::chrono::steady_clock::now();
            if (!r.has_value() && ok)
                std::printf("  [%s] %s\n", c.label,
                            r.error().render().substr(0, 160).c_str());
            ok = ok && r.has_value();
            us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }
        std::printf("%-18s %9ld %9ld %9ld  %s\n", c.label, pct(us, .5), pct(us, .9),
                    pct(us, 1.0), ok ? "ok" : "ERR");
    }
    return 0;
}
