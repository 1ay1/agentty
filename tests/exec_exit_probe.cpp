// exec_exit_probe — does a sandboxed command return when it exits, or at its
// timeout? Times trivial commands through the shipped path (saved policy ->
// set_config -> init -> run_shell_command), with the sandbox on and off.
//
//     cmake --build build --target exec_exit_probe && ./build/exec_exit_probe

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/domain/sandbox_config.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace sb = agentty::tools::util::sandbox;

// argv: [workspace] [on|off] [syscall_mode]
int main(int argc, char** argv) {
    const std::string ws = argc > 1 ? argv[1] : std::filesystem::current_path().string();
    agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), ws);
    agentty::sandbox_cfg::Config cfg;
    cfg.configured = true;
    if (argc > 3)
        cfg.syscall_mode = static_cast<decltype(cfg.syscall_mode)>(std::atoi(argv[3]));
    sb::set_config(cfg);
    const bool on = !(argc > 2 && std::string{argv[2]} == "off");
    sb::init(on ? sb::Mode::On : sb::Mode::Off);
    std::printf("sandbox %s: %s\n", on ? "on" : "off", sb::describe_state().c_str());

    const char* cmds[] = {
        "true",
        "echo hi",
        "echo hi >/dev/null 2>&1 </dev/null",
        "sleep 0.2; echo done",
        "(sleep 30 &) ; echo bg-left-running",   // a grandchild keeps the pipe
    };
    for (const char* c : cmds) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = sb::run_shell_command(c, 4096, std::chrono::seconds{8});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::string out = r.output;
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (out.size() > 60) out = out.substr(0, 60) + "...";
        std::printf("  %6lld ms  exit=%d timeout=%d  %-40s -> %s\n",
                    static_cast<long long>(ms), r.exit_code, int(r.timed_out), c, out.c_str());
    }
    return 0;
}
