// agentty has TWO sandbox implementations, and both must agree.
//
//   agentty::tools::util::sandbox  — lifecycle hooks, external ACP agents
//   mcp::tools::util::sandbox      — shell, diagnostics, git, process_start
//
// They are initialized separately (main.cpp, then wire_mcp_runtime) and each
// probes independently. Porting a backend into one and not the other is the
// failure this pins: the banner would describe one engine while every hook
// ran under another — the same shape as issue #21, where "sandbox: active"
// was printed on a host with no working sandbox.
//
// So: whatever backend one selects, the other must select too, under every
// environment the user can set.

#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"   // Config's default backend
#include "agentty/provider/prompt.hpp"          // default_system_prompt — the sandbox stanza
#include "agentty/tool/mcp_tools_bridge.hpp"
#include <mcp/tools/util/sandbox.hpp>

// Linux-only, like claybin's apply step. The userns case below compiles a
// plan for a DESCRIBED host, which is portable, but the policy types it needs
// are not.
#if defined(__linux__)
#include <claybin/plan/compile.hpp>
#include <claybin/policy/policy.hpp>
#include <claybin/policy/profiles.hpp>
#endif

#include <cstdlib>
#include <string>
#include <string_view>

namespace ag = agentty::tools::util::sandbox;
namespace mc = mcp::tools::util::sandbox;
namespace sandbox_cfg = agentty::sandbox_cfg;

namespace {

// The two enums are distinct types with the same meaning; compare by name so
// a reordering of either cannot make a mismatch look like agreement.
std::string name_of(ag::Backend b) {
    switch (b) {
        case ag::Backend::None:        return "none";
        case ag::Backend::Bwrap:       return "bwrap";
        case ag::Backend::SandboxExec: return "sandbox-exec";
    }
    return "?";
}
std::string name_of(mc::Backend b) {
    switch (b) {
        case mc::Backend::None:        return "none";
        case mc::Backend::Bwrap:       return "bwrap";
        case mc::Backend::SandboxExec: return "sandbox-exec";
    }
    return "?";
}

}  // namespace

#if defined(__linux__)
TEST_CASE("sandbox: claybin does not need a user namespace to be useful") {
    // The Ubuntu 24.04 case (KhazAkar's issue). That host ships an AppArmor
    // profile denying the uid_map write to unconfined binaries, so bwrap dies
    // with "setting up uid map: permission denied" and agentty reported no
    // backend at all -- the default is bwrap, bwrap failed, nothing looked
    // further.
    //
    // claybin's availability used to copy bwrap's precondition
    // (user_namespaces && mount_namespaces), which meant copying bwrap's
    // failure and made the second backend useless on exactly the host that
    // needed it. landlock and seccomp are UNPRIVILEGED mechanisms: neither
    // needs a namespace, and AppArmor's userns restriction does not touch
    // them.
    //
    // Measured through compile() on a described host, because this machine
    // has working namespaces and cannot be made not to. What survives with
    // userns off:
    //
    //   filesystem.read/write/exec  strong   landlock
    //   syscall.filter              strong   seccomp-bpf
    //   resource.memory             strong   cgroup2 memory.max
    //   privilege.drop              strong   no_new_privs + empty bounding set
    //
    // ...and what does not: network.isolation and process.isolation, both of
    // which genuinely need namespaces. That is a real downgrade, reported per
    // capability rather than papered over.
    using namespace ::clay;

    auto host = probe_host();
    host.user_namespaces = false;
    host.mount_namespaces = false;
    host.pid_namespaces = false;
    host.net_namespaces = false;
    host.uts_namespaces = false;

    // Skip where the machine cannot answer the question either way.
    if (host.landlock_abi == 0 && !host.seccomp) return;

    auto d = Policy<Draft>{};
    d = std::move(d).ro_bind("/usr", "/usr");
    d = std::move(d).bind("/tmp", "/tmp");
    d = std::move(d).syscall_profile(profiles::compiler_with_network());

    auto compiled = compile(std::move(d).seal(), host);
    REQUIRE(compiled.has_value());

    // The filesystem boundary is the one that matters: it is what keeps an
    // approved command out of ~/.ssh. If this is `none`, claybin has nothing
    // to offer on such a host and the availability floor below is wrong.
    if (host.landlock_abi > 0) {
        CHECK(compiled->guarantees.strength(CapId::fs_read) != Enforcement::none);
        CHECK(compiled->guarantees.strength(CapId::fs_write) != Enforcement::none);
    }
    if (host.seccomp)
        CHECK(compiled->guarantees.strength(CapId::syscall_filter) !=
              Enforcement::none);

    // And the honest part: namespace-backed walls are gone, not silently
    // claimed. A report that said `strong` here would be the exact lie this
    // subsystem exists to prevent.
    CHECK(compiled->guarantees.strength(CapId::proc_isolation) == Enforcement::none);
}
#endif  // __linux__

#if defined(__linux__)
TEST_CASE("sandbox: a microvm request is REFUSED, never downgraded") {
    // The invariant that makes "we do not implement kernel isolation" an honest
    // position rather than a gap.
    //
    // If compile() silently downgraded Isolation::microvm to a process sandbox,
    // a caller asking for a separate kernel would get a namespace and be told
    // nothing -- their threat model would say "separate kernel" while the
    // reality said "shared". That is the worst failure this subsystem could
    // have, and it is worse than not having the feature at all.
    //
    // Pinned from agentty rather than trusted, because it is a one-line check
    // in a submodule that a well-meaning "make the sandbox more forgiving"
    // change could delete, and nothing on this side would notice.
    using namespace ::clay;
    using namespace ::clay::literals;

    auto d = Policy<Draft>{};
    d = std::move(d).ro_bind("/usr", "/usr");
    d = std::move(d).isolation(Isolation::microvm);

    auto compiled = compile(std::move(d).seal(), probe_host());
    REQUIRE(!compiled.has_value());
    // And the reason names the missing piece, so the error is actionable
    // rather than a bare refusal.
    CHECK(std::string_view{compiled.error().mechanism}.find("microvm") !=
          std::string_view::npos);
}

TEST_CASE("sandbox: the kernel-isolation gap says whose fault it is") {
    // host.kernel_isolation is `none` for two very different reasons, and a
    // caller deciding whether to care deserves the real one: "we did not build
    // it" versus "this host could not run it anyway". A flat "process-backend"
    // invited the first reading, which is wrong on most CI runners.
    using namespace ::clay;

    auto report_with = [](bool kvm) {
        auto host = probe_host();
        host.kvm = kvm;
        // A policy complete enough to compile. A bare ro_bind is not: claybin
        // refuses a draft with no root, no /proc and no workdir, which is
        // correct of it and was my first mistake here.
        auto d = Policy<Draft>{};
        d = std::move(d).ro_bind("/usr", "/usr");
        d = std::move(d).proc_fs("/proc");
        d = std::move(d).dev_fs("/dev");
        d = std::move(d).tmpfs("/tmp", Bytes{64ull << 20});
        d = std::move(d).workdir("/");
        d = std::move(d).syscall_profile(profiles::compiler_with_network());
        auto compiled = compile(std::move(d).seal(), host);
        REQUIRE(compiled.has_value());
        // Always `none` on a process backend, whatever the host can do. That
        // part is machine-checked by claybin's own fuzzer too.
        CHECK(compiled->guarantees.strength(CapId::host_kernel_isolation) ==
              Enforcement::none);
        return std::string{
            compiled->guarantees.mechanism(CapId::host_kernel_isolation)};
    };

    CHECK(report_with(true).find("kvm available") != std::string::npos);
    CHECK(report_with(false).find("no kvm") != std::string::npos);
}
#endif  // __linux__

TEST_CASE("sandbox: wiring the runtime hands mcp OUR sandbox") {
    // The hook is the mechanism the parity check below now rests on, so it
    // gets its own case: if wire_mcp_runtime ever stops installing it, this
    // fails with a clear cause instead of the parity case failing with a
    // confusing one.
    //
    // The gap it closes: mcp-cpp probes for its OWN backend, and it cannot
    // reach claybin (standalone library, our submodule). So before the hook,
    // `shell` ran under mcp's bwrap while agentty's hooks and the settings
    // pane used claybin -- two engines, one banner, and a pane configuring a
    // boundary the tool the user actually invokes never read.
    agentty::tools::wire_mcp_runtime("auto");
    CHECK(mc::has_host_sandbox());

    // And "off" must NOT install it: with no sandbox requested there is
    // nothing to delegate, and a hook that declined every call would just be
    // an indirection.
    agentty::tools::wire_mcp_runtime("off");
    CHECK(!mc::has_host_sandbox());
}

TEST_CASE("sandbox: claybin is the default, and bwrap is still the fallback") {
    // The default flipped to claybin because the difference is MEASURED, not
    // argued: agentty's bwrap path emits no --seccomp, no cgroup limits and no
    // landlock, so claybin is strictly stronger on syscall.filter, all three
    // resource caps, filesystem.exec and brokering. There is no capability
    // where bwrap wins (tests/sandbox_audit.cpp prints the report).
    //
    // What this case actually guards is the OTHER half -- the reason bwrap was
    // kept rather than deleted. The two engines fail on DIFFERENT hosts:
    // bwrap dies where unprivileged userns is denied (Ubuntu 24.04 AppArmor),
    // and claybin needs landlock + seccomp + cgroup2, which a RHEL kernel
    // older than 5.13 does not have. Delete bwrap and that second host does
    // not get claybin -- it gets Backend::None. Trading a weaker sandbox for
    // NO sandbox is the issue #21 failure in a new hat, so the fallback is a
    // security property and belongs under test.
    ag::reset_config_for_test();

    // An untouched config asks for claybin.
    const sandbox_cfg::Config fresh;
    CHECK(fresh.backend == sandbox_cfg::LinuxBackend::Claybin);

    // And the engine actually selected is never None on a host that can run
    // EITHER backend. This is the fallback: whichever engine the preference
    // names, a usable host must still end up sandboxed.
    //
    // Deliberately NOT asserting that mcp's side agrees here. ag::init()
    // re-probes only agentty's implementation, so comparing the two after it
    // would be testing an ordering artifact of this file rather than a
    // property of the system -- the two are wired together in
    // wire_mcp_runtime(), and the case that owns that claim is "the two
    // implementations select the SAME backend" below.
    ag::init(ag::Mode::Auto);
    if (ag::is_active())
        CHECK(ag::detected_backend() != ag::Backend::None);
}

TEST_CASE("sandbox: asking for claybin on a host that refuses it yields bwrap, not None") {
    // The fallback DIRECTION, which is the whole reason flipping the default
    // is safe. Opting into (or now, defaulting to) the newer backend must
    // never cost someone their sandbox: a host where claybin cannot build its
    // walls has to land on bwrap rather than on nothing.
    //
    // Checked through the public surface rather than by forcing the probe,
    // because the probe is a real fork+uid_map attempt and faking it would
    // test the fake.
    ag::reset_config_for_test();
    ag::prefer_linux_backend(ag::LinuxPreference::Claybin);
    ag::init(ag::Mode::Auto);

#if defined(__linux__)
    if (ag::is_active()) {
        const auto b = ag::detected_backend();
        // Exactly one of the two Linux engines. Never None while active, and
        // never SandboxExec on Linux.
        CHECK((b == ag::Backend::Claybin || b == ag::Backend::Bwrap));
    }
#endif
}

TEST_CASE("sandbox: the model is TOLD it is sandboxed") {
    // A model that does not know it is sandboxed misdiagnoses every wall it
    // hits: EPERM becomes "your proxy is blocking this", a blocked connect
    // becomes "check your firewall", and it either sends the user chasing a
    // phantom or retries a call the kernel will never allow.
    //
    // So the <environment> stanza says the boundary exists. This pins that it
    // appears when sandboxed and -- just as importantly -- does NOT when the
    // user turned sandboxing off, because a prompt claiming walls that are not
    // there is the same lie in the other direction.
    ag::reset_config_for_test();
    ag::init(ag::Mode::Off);
    {
        const auto p = agentty::provider::default_system_prompt();
        CHECK(p.find("sandbox: ON") == std::string::npos);
    }

    ag::reset_config_for_test();
    ag::init(ag::Mode::Auto);
    if (ag::is_active()) {
        const auto p = agentty::provider::default_system_prompt();
        CHECK(p.find("sandbox: ON") != std::string::npos);
        // The three things that stop the misdiagnosis: where it can write,
        // that a denial is the sandbox, and that retrying is wrong.
        CHECK(p.find("writable") != std::string::npos);
        CHECK(p.find("not a broken tool") != std::string::npos);
        CHECK(p.find("Do NOT retry") != std::string::npos);
    }
}

TEST_CASE("sandbox: a denial is explained where the model is looking") {
    // The system prompt is thousands of tokens away by the time an error
    // arrives. The annotation goes on the OUTPUT, which is what the model is
    // actually reading when it decides whether to retry.
    //
    // Driven through the real runner so this covers the wiring, not just the
    // string: a command that cannot read a host path under any policy.
    ag::reset_config_for_test();
    ag::init(ag::Mode::Auto);
    if (!ag::is_active()) return;   // no backend here; nothing to annotate

    auto r = ag::run_shell_command("cat /etc/shadow", 64 * 1024,
                                   std::chrono::seconds{20});
    if (r.exit_code != 0 &&
        r.output.find("[sandbox]") != std::string::npos) {
        // When the annotation fires it must carry the instruction, not just
        // the diagnosis -- a note that only explains still leaves the model
        // free to retry.
        CHECK(r.output.find("Do not retry") != std::string::npos);
        CHECK(r.output.find("firewall") != std::string::npos);
    }
    // Not asserted: that it ALWAYS fires. Whether /etc/shadow is readable is a
    // property of the host's policy, and a test that demanded a denial would
    // fail on a machine configured differently rather than finding a bug.
}

TEST_CASE("sandbox: a SUCCEEDING command is never annotated") {
    // The note has to stay rare to stay read. Putting it on every command --
    // or on every failure, denial or not -- is how an explanation becomes
    // noise the model learns to skip.
    ag::reset_config_for_test();
    ag::init(ag::Mode::Auto);
    if (!ag::is_active()) return;

    auto r = ag::run_shell_command("echo hello", 64 * 1024,
                                   std::chrono::seconds{20});
    CHECK(r.output.find("[sandbox]") == std::string::npos);
}

TEST_CASE("sandbox: both implementations know the same backends") {
    // A backend added to one side only is the bug. This is a compile-time
    // check in practice — a backend missing from either enum won't build in
    // name_of — but assert the mapping too, so a silent renumbering is caught.
    CHECK(name_of(ag::Backend::Bwrap)       == name_of(mc::Backend::Bwrap));
    CHECK(name_of(ag::Backend::SandboxExec) == name_of(mc::Backend::SandboxExec));
    CHECK(name_of(ag::Backend::None)        == name_of(mc::Backend::None));
}

TEST_CASE("sandbox: the two implementations select the SAME backend") {
    // Both probe the live host. Whatever this machine supports, they must
    // agree — otherwise hooks and tools run under different confinement and
    // the status banner describes only one of them.
    (void)ag::init(ag::Mode::Auto);
    // Through the REAL bridge, not mc::init directly: wire_mcp_runtime is
    // what a running agentty calls, and it is where the host sandbox gets
    // installed. Calling mc::init here would test a path production never
    // takes and miss the very wiring this case exists to check.
    agentty::tools::wire_mcp_runtime("auto");

    // agentty installs itself as mcp's HOST SANDBOX, which settles this
    // invariant by construction rather than by coincidence: mcp does not
    // pick an engine at all, it hands every command to ours. Two independent
    // probes agreeing is luck that holds until the two probes drift; one
    // engine cannot disagree with itself.
    //
    // That delegation is also the only way the settings pane can mean
    // anything. mcp-cpp is a standalone library and claybin is OUR submodule,
    // so mcp cannot reach claybin on its own -- without the hook the `shell`
    // tool ran under mcp's built-in bwrap while the pane configured claybin,
    // and one banner described both. Which is exactly the issue #21 shape
    // this file exists to pin.
    if (mc::has_host_sandbox()) {
        // Delegation is in force. Nothing left to compare: the tools run in
        // whatever ag:: selected, whatever that is.
        CHECK(ag::is_active() == mc::is_active());
        return;
    }

    // No host sandbox (a build or platform where we install none): fall back
    // to requiring the two independent probes to have landed in the same
    // place.
    const auto a = name_of(ag::detected_backend());
    const auto b = name_of(mc::detected_backend());
    if (a != b)
        std::printf("MISMATCH: hooks/ACP use %s, tools use %s\n",
                    a.c_str(), b.c_str());
    CHECK(a == b);

    // And they must agree on whether a sandbox is active at all, which is
    // what the startup banner reports.
    CHECK(ag::is_active() == mc::is_active());
}
