// What does agentty ACTUALLY enforce, capability by capability, on this host?
//
// Not a scratch policy -- the real posture build_claybin_posture() produces,
// so the answer is about agentty rather than about a hand-written example.
// Every `none` here is either a deliberate trade or a gap, and the point of
// the tool is to stop guessing which.
//
// Run it when touching the sandbox:
//   cmake --build build --target sandbox_audit && ./build/sandbox_audit
#include "agentty/util/io.hpp"
#include "agentty//tool/util/sandbox_claybin.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"

#include <claybin/plan/compile.hpp>
#include <claybin/policy/policy.hpp>
#if defined(_WIN32)
#include <claybin/windows/backend.hpp>
#endif
#if defined(__APPLE__)
#include <claybin/macos/backend.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace cb = agentty::tools::util::sandbox::claybin_backend;
namespace cfgn = agentty::sandbox_cfg;
namespace sb = agentty::tools::util::sandbox;

namespace agentty::tools::util { void set_workspace_root(Io, std::filesystem::path p); }

namespace {

const char* strength(::clay::Enforcement e) {
    switch (e) {
        case ::clay::Enforcement::strong:   return "strong  ";
        case ::clay::Enforcement::partial:  return "partial ";
        case ::clay::Enforcement::advisory: return "advisory";
        case ::clay::Enforcement::none:     return "NONE    ";
    }
    return "?       ";
}

// No HostCapabilities parameter: it was always unused (`(void)host`), and once
// the probe became platform-dependent it could not be ONE type anyway --
// linux has clay::HostCapabilities, macOS has clay::macos::HostCapabilities.
// describe() re-probes internally through the backend that will actually run
// the policy, which is the only answer that was ever correct here.
void audit(const char* label, const cb::Posture& p) {
    std::printf("\n=== %s\n", label);
    auto rep = cb::describe(p);
    if (!rep.ok) {
        std::printf("  compile refused: %s\n", rep.detail.c_str());
        return;
    }
    for (const auto& w : rep.walls)
        std::printf("  %-22s %s %s\n", w.name.c_str(),
                    w.strength == "strong"      ? "strong  "
                    : w.strength == "partial"   ? "partial "
                    : w.strength == "advisory"  ? "advisory"
                                                : "NONE    ",
                    w.mechanism.c_str());
}

void audit_cfg(const char* label, const cfgn::Config& cfg) {
    sb::reset_config_for_test();
    sb::set_config(cfg);
    audit(label, sb::claybin_posture_for_test());
}

}  // namespace

int main(int argc, char** argv) {
    const char* ws = "/tmp/agentty-audit-ws";
    std::filesystem::create_directories(ws);
    agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), ws);

    // The host facts, per platform. Printing the Linux probe unconditionally
    // was actively misleading on a mac: every field reads 0 (there are no
    // namespaces, no landlock, no cgroups there) which looks like a host with
    // NOTHING, while the capability tables below report seatbelt walls doing
    // real work. Two contradictory answers on one screen, and the wrong one
    // first.
#if defined(__APPLE__)
    const auto mhost = ::clay::macos::probe_host();
    std::printf("host: darwin=%u seatbelt=%d\n", mhost.darwin_major,
                (int)mhost.seatbelt);
    std::printf("      sbpl: paths=%d net=%d proc=%d\n",
                (int)mhost.sbpl_path_filters, (int)mhost.sbpl_network_filters,
                (int)mhost.sbpl_process_filters);
    std::printf("      rlimits=%d\n", (int)mhost.rlimits);
    // Same rationale as kvm below: the difference between "we do not implement
    // a microvm" and "this host could not run one anyway".
    std::printf("      hypervisor=%d%s\n", (int)mhost.hypervisor,
                mhost.hypervisor ? "  (a microvm backend COULD run here)" : "");
#else
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
#endif

    // 1. The SHIPPED default: what a user who never opens the pane gets.
    //    This is the number that matters most, because it is what almost
    //    everyone runs.
    // 1. What a user gets if they never open the pane at all: the shipped
    //    default, through the SAME config -> posture builder the runtime uses.
    {
        const cfgn::Config def{};
        audit_cfg("shipped default (no pane visit)", def);
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
        audit_cfg(label.c_str(), cfg);
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
        audit_cfg("everything the pane can ask for", cfg);
    }

    sb::reset_config_for_test();

#if defined(_WIN32)
    // 4. LIVE spawn, through the shipped path, with the shipped posture. The
    // audit above grades compile-time walls; this answers the question the
    // walls cannot: does a real child survive them on THIS host? The exec
    // chain failures (0xc0000142 during dll init) never showed up in any
    // compile-time report, so the audit now measures the runtime too.
    //
    // argv[1] optionally isolates ONE variable per run (the rest stays the
    // shipped default):
    //   no-net        net_mode = None (no capability SIDs in the token)
    //   no-wx         wx_protect = false
    const char* isolate = argc > 1 ? argv[1] : "";
    {
        sb::set_config(cfgn::Config{});
        auto posture = sb::claybin_posture_for_test();
        if (std::string_view{isolate} == "no-net") posture.net_mode = 1;
        if (std::string_view{isolate} == "no-wx") posture.wx_protect = false;
        std::printf("\n=== live spawn (shipped posture%s%s)\n",
                    *isolate ? " isolate=" : "", isolate);
        std::fflush(stdout);
        // the probe verdict IN THIS PROCESS, immediately before the spawn --
        // caps_print (standalone) reports restricted_tokens=0 stably while the
        // spawn below ran with rt_used=1; this pinpoints WHICH call disagrees.
        {
            const auto pv = ::clay::windows::probe_host();
            std::printf("  probe: ac=%d rt=%d mit=%d build=%u\n",
                        pv.app_container, pv.restricted_tokens,
                        pv.mitigation_policies, pv.build);
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE r = nullptr, w = nullptr;
        if (!::CreatePipe(&r, &w, &sa, 0)) {
            std::printf("  pipe: gle=%lu\n", ::GetLastError());
            return 1;
        }
        const int wfd = static_cast<int>(reinterpret_cast<intptr_t>(w));
        auto res = cb::spawn_argv(posture,
            {"cmd.exe", "/d", "/s", "/c", "echo audit-live"}, wfd, wfd, {});
        ::CloseHandle(w);  // our write end goes, or the reader never sees EOF
        if (!res.started) {
            std::printf("  spawn failed: %s\n", res.start_error.c_str());
            ::CloseHandle(r);
            return 1;
        }
        std::printf("  pid=%d\n", res.pid);
        char buf[512]{};
        DWORD n = 0;
        std::string out;
        while (::ReadFile(r, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
        ::CloseHandle(r);
        DWORD code = 0;
        HANDLE ph = reinterpret_cast<HANDLE>(
            static_cast<intptr_t>(res.pidfd));
        ::WaitForSingleObject(ph, 20000);
        ::GetExitCodeProcess(ph, &code);
        std::printf("  exit=%lu (%s) output='%.*s'\n", code,
                    code == 0 ? "RUNS" : "DIED  (0x hex below)",
                    static_cast<int>(out.size()), out.c_str());
        std::printf("  exit-hex=0x%08lx\n", code);
        if (res.kill_tree) (void)res.kill_tree();
        // NOTE: pidfd is a raw handle borrowed for the wait above; agentty's
        // adapter owns it via the job/kill_tree closure in real runs. The
        // audit is a tool, not a leak-free harness.
    }
#endif

    std::filesystem::remove_all(ws);
}
