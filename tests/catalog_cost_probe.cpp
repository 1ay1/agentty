// How expensive is catalog_block() now that it resolves trust?
//
// It runs on EVERY turn (provider::system_prompt_for → default_system_prompt),
// and the trust gate added a file read plus a SHA-256 over every skill body
// to that path. This measures it rather than assuming.

#include "agentty/tool/skills.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace agentty::tools::skills;

int main() {
    const auto base = fs::temp_directory_path() / "agentty-catalog-bench";
    fs::remove_all(base);
    const auto home = base / "home";
    const auto root = home / ".agentty" / "skills";

    // 40 skills with realistic bodies (~4 KB each — a real SKILL.md).
    const std::string body(4096, 'x');
    for (int i = 0; i < 40; ++i) {
        const auto d = root / ("skill" + std::to_string(i));
        fs::create_directories(d);
        std::ofstream(d / "SKILL.md")
            << "---\nname: skill" << i
            << "\ndescription: does thing number " << i << "\n---\n"
            << body << "\n";
    }

    ::setenv("HOME", home.c_str(), 1);
    ::setenv("AGENTTY_HOME", (home / ".agentty").c_str(), 1);

    // Warm the skills cache so we measure the catalog, not discovery.
    (void)catalog_block();

    constexpr int kRuns = 200;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t sink = 0;
    for (int i = 0; i < kRuns; ++i) sink += catalog_block().size();
    const auto t1 = std::chrono::steady_clock::now();

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const auto per = static_cast<long long>(us / kRuns);
    std::printf("catalog_block: %lld us/call over %d calls (40 skills, 4KB bodies)\n",
                per, kRuns);
    (void)sink;

    // A BUDGET, not just a number. catalog_block() runs on every turn, on
    // the path between the user pressing enter and the request going out.
    // It was 3400 us/call when all() re-parsed every SKILL.md per call and
    // threw the parse away; the stat-only signature pass took it to ~500.
    //
    // 1500 is deliberately loose — this is a debug build on shared CI, and
    // a flaky perf test gets deleted rather than fixed. It catches the
    // REGRESSION SHAPE (re-parsing the library per turn), not jitter.
    constexpr long long kBudgetUs = 1500;
    const bool ok = per < kBudgetUs;
    if (!ok) {
        std::printf("FAIL: %lld us/call exceeds the %lld us budget.\n", per, kBudgetUs);
        std::printf("  something on the per-turn path is doing work again —\n");
        std::printf("  check all() still skips the parse on an unchanged signature.\n");
    }

    fs::remove_all(base);
    std::printf("%s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
