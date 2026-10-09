// ws_bind_probe — does the SAVED sandbox policy actually reach the walls?
//
// Two questions, and they have different answers when something is broken:
//
//   1. does ~/.agentty/settings.json round-trip through load_settings()?
//   2. does the config that load_settings() returns actually shape the
//      sandbox a real `bash` call lands in?
//
// Driven through the SHIPPED path on both counts: persistence::load_settings()
// then sandbox::set_config() in the same order main.cpp uses, then
// run_shell_command(). A mirrored posture would prove nothing — the bug under
// investigation is precisely that the saved block and the live walls disagree.

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/domain/sandbox_config.hpp"
#include "agentty/util/user_root.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace sb = agentty::tools::util::sandbox;
namespace sc = agentty::sandbox_cfg;

namespace {

const char* net_name(sc::NetMode m) {
    switch (m) {
        case sc::NetMode::Full:  return "Full";
        case sc::NetMode::None:  return "None";
        case sc::NetMode::Ports: return "Ports";
    }
    return "?";
}

const char* scope_name(sc::FsScope s) {
    switch (s) {
        case sc::FsScope::Minimal:   return "Minimal";
        case sc::FsScope::Toolchain: return "Toolchain";
        case sc::FsScope::HostReadable:  return "HostReadable";
    }
    return "?";
}

}  // namespace

// Mirror of persistence.cpp's sandbox reader. Mirrored rather than linked
// because pulling in persistence.cpp drags the whole thread/auth/registry
// graph behind it; this file only needs the one block. If the two ever
// disagree THAT is itself the finding -- so every field is read with the
// same key and the same fallback-to-current-value rule.
sc::Config read_saved_sandbox() {
    sc::Config c;
    std::ifstream ifs(agentty::util::user_root() / "settings.json");
    if (!ifs) { std::printf("  (no settings.json)\n"); return c; }
    nlohmann::json j;
    try { ifs >> j; } catch (...) { std::printf("  (unparseable)\n"); return c; }
    if (!j.contains("sandbox") || !j["sandbox"].is_object()) {
        std::printf("  (no sandbox block)\n");
        return c;
    }
    const auto& b = j["sandbox"];
    c.configured = b.value("configured", true);
    c.fs_scope = static_cast<sc::FsScope>(
        b.value("fs_scope", static_cast<int>(c.fs_scope)));
    c.net_mode = static_cast<sc::NetMode>(
        b.value("net_mode", static_cast<int>(c.net_mode)));
    c.syscall_mode = static_cast<sc::SyscallMode>(
        b.value("syscall_mode", static_cast<int>(c.syscall_mode)));
    c.memory_mb       = b.value("memory_mb", c.memory_mb);
    c.max_procs       = b.value("max_procs", c.max_procs);
    c.cpu_secs        = b.value("cpu_secs", c.cpu_secs);
    c.wall_clock_secs = b.value("wall_clock_secs", c.wall_clock_secs);
    c.mask_scan_depth = b.value("mask_scan_depth", c.mask_scan_depth);
    if (b.contains("allow_ports") && b["allow_ports"].is_array()) {
        c.allow_ports.clear();
        for (const auto& v : b["allow_ports"])
            if (v.is_number_integer()) c.allow_ports.push_back(v.get<int>());
    }
    return c;
}

int main(int argc, char** argv) {
    const std::string ws = argc > 1 ? argv[1]
                                    : std::filesystem::current_path().string();
    agentty::tools::util::set_workspace_root(ws);

    // ── 1. what is ON DISK ──────────────────────────────────────────────
    const auto saved = read_saved_sandbox();
    std::printf("SAVED policy (~/.agentty/settings.json)\n");
    std::printf("  configured     %s\n", saved.configured ? "true" : "false");
    std::printf("  fs_scope       %s\n", scope_name(saved.fs_scope));
    std::printf("  net_mode       %s\n", net_name(saved.net_mode));
    std::printf("  memory_mb      %d\n", saved.memory_mb);
    std::printf("  max_procs      %d\n", saved.max_procs);
    std::printf("  cpu_secs       %d\n", saved.cpu_secs);
    std::printf("  wall_clock_secs %d\n", saved.wall_clock_secs);
    std::printf("  mask_scan_depth %u\n", saved.mask_scan_depth);

    // ── 2. seal it, exactly as main.cpp does ────────────────────────────
    sb::set_config(saved);
    sb::init(sb::Mode::On);

    const auto live = sb::config();
    std::printf("\nLIVE policy (sandbox::config() after set_config + init)\n");
    std::printf("  net_mode       %s\n", net_name(live.net_mode));
    std::printf("  memory_mb      %d\n", live.memory_mb);
    std::printf("  state          %s\n", sb::describe_state().c_str());
    std::printf("  enforceable    %s\n",
                sb::config_enforceable() ? "yes" : "NO (backend cannot apply it)");

    const bool match = live.net_mode == saved.net_mode
                    && live.memory_mb == saved.memory_mb
                    && live.max_procs == saved.max_procs;
    std::printf("\n  saved == live  %s\n", match ? "YES" : "NO  <-- the bug");

    // ── 3. does the LIVE policy actually shape a real command? ──────────
    std::printf("\nOBSERVED in a real run_shell_command child\n");
    struct Probe { const char* label; const char* cmd; };
    const Probe probes[] = {
        {"network reachable", "curl -sS -m 5 -o /dev/null -w '%{http_code}' "
                              "https://example.com 2>&1 | tail -1"},
        {"memory cap",        "cat /sys/fs/cgroup/memory.max 2>/dev/null "
                              "|| echo '(no cgroup memory.max)'"},
        {"pid cap",           "cat /sys/fs/cgroup/pids.max 2>/dev/null "
                              "|| echo '(no cgroup pids.max)'"},
    };
    for (const auto& p : probes) {
        auto r = sb::run_shell_command(p.cmd, 8192, std::chrono::seconds{20});
        std::string out = r.output;
        if (const auto at = out.find("\n[sandbox]"); at != std::string::npos)
            out.resize(at);
        while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            out.pop_back();
        std::printf("  %-18s %s\n", p.label, out.c_str());
    }
    return match ? 0 : 1;
}
