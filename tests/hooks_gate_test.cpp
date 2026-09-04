// hooks_gate_test.cpp — the lifecycle-hooks CONSENT GATE. The security
// property this pins is the whole design: a hooks file that has not been
// explicitly approved NEVER executes, and any byte change to an approved
// file re-gates it. Also covers the pre_tool block semantics and the
// post_tool fire-and-forget contract.
//
// Runs in a temp sandbox CWD + HOME (approvals live under $HOME). Hook
// commands write marker files so execution (or its absence) is observable.

#include "agentty/tool/hooks.hpp"
#include "agentty/auth/auth.hpp"

#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace hooks = agentty::tools::hooks;

namespace {

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

[[nodiscard]] std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
}

// Approve the CURRENT bytes of the hooks file by writing the approval map
// directly (the interactive `agentty hooks approve` does the same store).
void approve_current(const fs::path& hooks_file, const fs::path& home) {
    std::string raw = read_all(hooks_file);
    std::error_code ec;
    auto abs = fs::weakly_canonical(hooks_file, ec);
    nlohmann::json j = nlohmann::json::object();
    j[(ec ? fs::absolute(hooks_file, ec) : abs).string()] =
        agentty::auth::sha256_hex(raw);
    write_file(home / ".agentty" / "hooks_approved.json", j.dump(2));
}

void unapproved_never_runs(const fs::path& sandbox) {
    std::println("--- unapproved_never_runs ---");
    const fs::path marker = sandbox / "pre_ran.marker";
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool", {{{"match", ""},
                                  {"run", "touch " + marker.string()}}}},
               }.dump());

    check(hooks::pending_approval(), "unapproved file reports pending");
    auto d = hooks::run_pre_tool("shell", "{}");
    check(!d.blocked, "unapproved hooks never block");
    check(!fs::exists(marker), "unapproved hook COMMAND NEVER EXECUTED");
    std::println("PASS\n");
}

void approved_runs_and_blocks(const fs::path& sandbox, const fs::path& home) {
    std::println("--- approved_runs_and_blocks ---");
    const fs::path marker = sandbox / "post_ran.marker";
    // pre_tool guard: block any bash whose args mention "rm -rf".
    // post_tool: drop a marker (observability for fire-and-forget).
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool",
                    {{{"match", "shell"},
                      {"run",
                       "grep -q 'rm -rf' \"$AGENTTY_HOOK_PAYLOAD_FILE\" "
                       "&& { echo 'dangerous command blocked'; exit 1; } "
                       "|| exit 0"}}}},
                   {"post_tool", {{{"match", "shell"},
                                   {"run", "touch " + marker.string()}}}},
               }.dump());
    approve_current(sandbox / ".agentty" / "hooks.json", home);
    check(!hooks::pending_approval(), "approved file no longer pending");

    // Benign call: allowed, post fires.
    auto ok = hooks::run_pre_tool("shell", R"({"command":"ls -la"})");
    check(!ok.blocked, "benign bash allowed");
    hooks::run_post_tool("shell", R"({"command":"ls"})", "listing…");
    check(fs::exists(marker), "post_tool hook executed for approved file");

    // Dangerous call: blocked with the hook's stdout as reason.
    auto bad = hooks::run_pre_tool("shell", R"({"command":"rm -rf /tmp/x"})");
    check(bad.blocked, "matching pre_tool guard blocks");
    check(bad.reason.find("dangerous command blocked") != std::string::npos,
          "hook stdout becomes the block reason (got '" + bad.reason + "')");

    // Non-matching tool: guard does not fire.
    auto other = hooks::run_pre_tool("read", R"({"path":"rm -rf"})");
    check(!other.blocked, "match=bash guard ignores the read tool");
    std::println("PASS\n");
}

void byte_change_regates(const fs::path& sandbox) {
    std::println("--- byte_change_regates ---");
    const fs::path marker = sandbox / "regate.marker";
    // Mutate the approved file (append a space — ANY byte change).
    const fs::path hf = sandbox / ".agentty" / "hooks.json";
    write_file(hf, nlohmann::json{
                       {"pre_tool", {{{"match", ""},
                                      {"run", "touch " + marker.string()}}}},
                   }.dump() + " ");
    check(hooks::pending_approval(),
          "byte change to an approved file re-gates (pending again)");
    (void)hooks::run_pre_tool("shell", "{}");
    check(!fs::exists(marker), "re-gated hook did not execute");
    std::println("PASS\n");
}

void kill_switch(const fs::path& sandbox, const fs::path& home) {
    std::println("--- kill_switch ---");
    const fs::path marker = sandbox / "killswitch.marker";
    write_file(sandbox / ".agentty" / "hooks.json",
               nlohmann::json{
                   {"pre_tool", {{{"match", ""},
                                  {"run", "touch " + marker.string()}}}},
               }.dump());
    approve_current(sandbox / ".agentty" / "hooks.json", home);
#ifndef _WIN32
    ::setenv("AGENTTY_NO_HOOKS", "1", 1);
#endif
    check(!hooks::pending_approval(), "kill switch silences pending notice");
    (void)hooks::run_pre_tool("shell", "{}");
    check(!fs::exists(marker), "AGENTTY_NO_HOOKS=1 disables approved hooks");
#ifndef _WIN32
    ::unsetenv("AGENTTY_NO_HOOKS");
#endif
    std::println("PASS\n");
}

} // namespace

TEST_CASE("hooks_gate") {
    const fs::path sandbox =
        fs::temp_directory_path() / ("agentty_hooks_test_" +
                                     std::to_string(::getpid()));
    const fs::path home = sandbox / "home";
    fs::create_directories(home);
    fs::current_path(sandbox);
#ifdef _WIN32
    _putenv_s("USERPROFILE", home.string().c_str());
#else
    ::setenv("HOME", home.string().c_str(), 1);
    // User-scope config resolves via util::user_root() ($AGENTTY_HOME,
    // else $HOME/.agentty) — point it at this test's home so the files
    // written under home/.agentty are the ones the loader reads.
    ::unsetenv("AGENTTY_HOME");   // fall back to $HOME/.agentty
#endif

    std::println("=== hooks_gate_test ===");
    unapproved_never_runs(sandbox);
    approved_runs_and_blocks(sandbox, home);
    byte_change_regates(sandbox);
    kill_switch(sandbox, home);

    std::error_code ec;
    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(sandbox, ec);
}
