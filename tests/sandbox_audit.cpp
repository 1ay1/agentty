// What does agentty ACTUALLY enforce, capability by capability, on this host?
//
// Not a scratch policy -- the real posture build_claybin_posture() produces,
// so the answer is about agentty rather than about a hand-written example.
// Every `none` here is either a deliberate trade or a gap, and the point of
// the tool is to stop guessing which.
//
// Run it when touching the sandbox:
//   cmake --build build --target sandbox_audit && ./build/sandbox_audit
#include "agentty/tool/util/sandbox_claybin.hpp"
#include "agentty/domain/sandbox_config.hpp"

#include <claybin/plan/compile.hpp>
#include <claybin/policy/policy.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace cb = agentty::tools::util::sandbox::claybin_backend;
namespace cfgn = agentty::sandbox_cfg;

namespace agentty::tools::util { void set_workspace_root(std::filesystem::path p); }

namespace {

const char* strength(::clay::Enforcement e) {
    switch (e) {
        case ::clay::Enforcement::strong:  return "strong ";
        case ::clay::Enforcement::partial: return "partial";
        case ::clay::Enforcement::none:    return "NONE   ";
    }
    return "?      ";
}

void audit(const char* label, const cb::Posture& p, ::clay::HostCapabilities host) {
    std::printf("\n=== %s\n", label);
    auto rep = cb::describe(p);
    if (!rep.ok) {
        std::printf("  compile refused: %s\n", rep.detail.c_str());
        return;
    }
    for (const auto& w : rep.walls)
        std::printf("  %-22s %s %s\n", w.name.c_str(),
                    w.strength == "strong"    ? "strong "
                    : w.strength == "partial" ? "partial"
                                              : "NONE   ",
                    w.mechanism.c_str());
    (void)host;
}

}  // namespace

int main() {
    const char* ws = "/tmp/agentty-audit-ws";
    std::filesystem::create_directories(ws);
    agentty::tools::util::set_workspace_root(ws);

    const auto host = ::clay::probe_host();
    std::printf("host: userns=%d mountns=%d pidns=%d netns=%d\n",
                (int)host.user_namespaces, (int)host.mount_namespaces,
                (int)host.pid_namespaces, (int)host.net_namespaces);
    std::printf("      landlock_abi=%u seccomp=%d notify=%d\n",
                host.landlock_abi, (int)host.seccomp,
                (int)host.seccomp_user_notif);
    std::printf("      cgroups=%d mem=%d pids=%d cpu=%d\n",
                (int)host.cgroups, (int)host.cgroup_memory,
                (int)host.cgroup_pids, (int)host.cgroup_cpu);

    // 1. The SHIPPED default: what a user who never opens the pane gets.
    //    This is the number that matters most, because it is what almost
    //    everyone runs.
    //
    //    The resource caps come from a default-constructed Config rather than
    //    being written out here, so this case cannot drift from what the
    //    product actually ships. Writing them by hand is how an audit ends up
    //    auditing itself.
    {
        const cfgn::Config def{};
        cb::Posture p;
        p.system_read_roots = {"/usr", "/bin", "/sbin", "/lib", "/lib64"};
        p.workspace = ws;
        p.cwd = ws;
        p.syscall_mode = static_cast<int>(def.syscall_mode);
        p.net_mode = static_cast<int>(def.net_mode);
        p.wx_protect = def.wx_protect;
        p.scope_ipc = def.scope_ipc;
        p.close_inherited_fds = def.close_inherited_fds;
        p.tmp_bytes = def.tmp_mb * 1024ull * 1024;
        p.memory_bytes = def.memory_mb * 1024ull * 1024;
        p.max_processes = def.max_procs;
        p.cpu_percent = def.cpu_percent;
        p.max_open_files = def.max_open_files;
        p.cpu_secs = def.cpu_secs;
        p.wall_clock_secs = def.wall_clock_secs;
        p.fake_hostname = def.fake_hostname;
        audit("shipped default (no pane visit)", p, host);
    }

    // 2. Everything the pane can ask for, turned up. The ceiling: if a
    //    capability is `none` HERE, agentty cannot deliver it at all and it is
    //    a genuine gap rather than a default.
    {
        cb::Posture p;
        p.system_read_roots = {"/usr", "/bin", "/sbin", "/lib", "/lib64"};
        p.workspace = ws;
        p.cwd = ws;
        p.syscall_mode = 2;          // strict
        p.net_mode = 1;              // none
        p.wx_protect = true;
        p.scope_ipc = true;
        p.close_inherited_fds = true;
        p.tmp_bytes = 64ull << 20;
        p.memory_bytes = 512ull << 20;
        p.max_processes = 256;
        p.cpu_percent = 100;
        p.max_open_files = 1024;
        p.cpu_secs = 60;
        p.wall_clock_secs = 120;
        p.fake_hostname = true;
        audit("everything the pane can ask for", p, host);
    }

    std::filesystem::remove_all(ws);
}
