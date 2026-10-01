// ws_bind_probe — does the workspace actually get bound, and is the rest of
// $HOME actually read-only?
//
// Runs the SHIPPED path (run_shell_command) with the user's real saved
// settings, not a hand-built posture, because the report under investigation
// claims the two disagree.

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/domain/sandbox_config.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace sb = agentty::tools::util::sandbox;

int main(int argc, char** argv) {
    const std::string ws = argc > 1 ? argv[1]
                                    : std::filesystem::current_path().string();
    agentty::tools::util::set_workspace_root(ws);

    agentty::sandbox_cfg::Config cfg;
    cfg.configured = true;                 // the shipped Balanced defaults
    sb::set_config(cfg);
    sb::init(sb::Mode::On);

    std::printf("workspace_root() = %s\n",
                agentty::tools::util::workspace_root().string().c_str());
    std::printf("sandbox state    = %s\n", sb::describe_state().c_str());
    std::printf("active           = %s\n\n", sb::is_active() ? "yes" : "no");

    struct Probe { const char* label; std::string cmd; };
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "/root";

    const Probe probes[] = {
        {"cwd",                "pwd"},
        {"workspace in mounts","grep -c \"" + ws + "\" /proc/self/mountinfo || true"},
        {"WRITE workspace",    "echo x > \"" + ws + "/WS_PROBE\" && echo OK && rm -f \"" + ws + "/WS_PROBE\""},
        {"WRITE ~/.config",    "echo x > \"" + home + "/.config/ESC_PROBE\" && echo ESCAPED && rm -f \"" + home + "/.config/ESC_PROBE\""},
        {"WRITE ~/Documents",  "echo x > \"" + home + "/Documents/ESC_PROBE\" && echo ESCAPED && rm -f \"" + home + "/Documents/ESC_PROBE\""},
        {"WRITE /var/tmp",     "echo x > /var/tmp/ESC_PROBE && echo ESCAPED && rm -f /var/tmp/ESC_PROBE"},
        {"WRITE ~/.gitconfig", "echo '# probe' >> \"" + home + "/.gitconfig\" && echo ESCAPED"},
        {"READ gh-notify ini", "cat \"" + home + "/.config/gh-issue-notify/config.ini\" | head -1"},
        {"READ ~/.ssh",        "ls \"" + home + "/.ssh\" | head -3"},
        {"TLS curl",           "curl -sS -o /dev/null -w 'http=%{http_code}' https://example.com 2>&1 | tail -1"},
        {"timeout works",      "timeout 1 sleep 5; echo \"exit=$?\""},
    };

    for (const auto& p : probes) {
        auto r = sb::run_shell_command(p.cmd, 64 * 1024,
                                       std::chrono::seconds{20});
        std::string out = r.output;
        while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            out.pop_back();
        if (out.size() > 200) out = out.substr(0, 200) + "…";
        std::printf("%-22s exit=%-4d %s\n", p.label, r.exit_code, out.c_str());
    }
    return 0;
}
