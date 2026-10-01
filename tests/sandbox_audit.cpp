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
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"

#include <claybin/plan/compile.hpp>
#include <claybin/policy/policy.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace cb = agentty::tools::util::sandbox::claybin_backend;
namespace cfgn = agentty::sandbox_cfg;
namespace sb = agentty::tools::util::sandbox;

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

void audit_cfg(const char* label, const cfgn::Config& cfg, ::clay::HostCapabilities host) {
    sb::reset_config_for_test();
    sb::set_config(cfg);
    audit(label, sb::claybin_posture_for_test(), host);
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
    // kvm is not used by the process backend. It is printed because it is the
    // difference between "we do not implement a microvm" and "this host could
    // not run one anyway" -- two very different answers to why
    // host.kernel_isolation is none.
    std::printf("      kvm=%d%s\n", (int)host.kvm,
                host.kvm ? "  (a microvm backend COULD run here)" : "");

    // 1. The SHIPPED default: what a user who never opens the pane gets.
    //    This is the number that matters most, because it is what almost
    //    everyone runs.
    // 1. What a user gets if they never open the pane at all: the shipped
    //    default, through the SAME config -> posture builder the runtime uses.
    {
        const cfgn::Config def{};
        audit_cfg("shipped default (no pane visit)", def, host);
    }

    // 2. Every named posture, again through the real builder rather than a
    //    mirror. If two labels collapse to the same walls here, the product's
    //    preset ladder has collapsed too.
    for (const auto posture : {cfgn::Posture::Permissive,
                               cfgn::Posture::Balanced,
                               cfgn::Posture::Hardened,
                               cfgn::Posture::Airgapped}) {
        auto cfg = cfgn::apply_posture(cfgn::Config{}, posture);
        cfg.configured = true;
        const std::string label = std::string{"preset: "} + cfgn::to_string(posture);
        audit_cfg(label.c_str(), cfg, host);
    }

    // 3. Everything the pane can ask for, turned up. The ceiling: if a
    //    capability is `none` HERE, agentty cannot deliver it at all and it is
    //    a genuine gap rather than a default.
    {
        cfgn::Config cfg;
        cfg.configured = true;
        cfg.syscall_mode = cfgn::SyscallMode::Strict;
        cfg.net_mode = cfgn::NetMode::None;
        cfg.wx_protect = true;
        cfg.scope_ipc = true;
        cfg.close_inherited_fds = true;
        cfg.tmp_mb = 64;
        cfg.memory_mb = 512;
        cfg.max_procs = 256;
        cfg.cpu_percent = 100;
        cfg.max_open_files = 1024;
        cfg.cpu_secs = 60;
        cfg.wall_clock_secs = 120;
        cfg.fake_hostname = true;
        audit_cfg("everything the pane can ask for", cfg, host);
    }

    sb::reset_config_for_test();
    std::filesystem::remove_all(ws);
}
