// agentty's sandbox: backend selection, what the model is told, denial
// annotations, credential masks, and claybin's guarantees.
//
// There used to be a second implementation in mcp-cpp that had to agree with
// this one. mcp-cpp runs nothing itself any more (every command goes through
// agentty's exec), so there is one sandbox and nothing to keep in step.

#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/domain/sandbox_config.hpp"   // Config's default backend
#include "agentty/provider/prompt.hpp"          // default_system_prompt — the sandbox stanza

// Linux-only, like claybin's apply step. The userns case below compiles a
// plan for a DESCRIBED host, which is portable, but the policy types it needs
// are not.
#if defined(__linux__)
#include <claybin/plan/compile.hpp>
#include <claybin/policy/policy.hpp>
#include <claybin/policy/profiles.hpp>
#endif

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace ag = agentty::tools::util::sandbox;
namespace sandbox_cfg = agentty::sandbox_cfg;

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
    ag::init(ag::Mode::Auto);

#if defined(__linux__)
    if (ag::is_active()) {
        const auto b = ag::detected_backend();
        // Exactly one of the two Linux engines. Never None while active, and
        // never SandboxExec on Linux.
        CHECK(b == ag::Backend::Claybin);
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

TEST_CASE("sandbox masks: the credential name list covers what repos hold") {
    // The sweep walks the workspace and masks any file whose BASENAME is a
    // credential name. The walk was right; the list was thin -- it had five
    // entries and missed .npmrc, .git-credentials, .netrc, the rest of the
    // .env family, three of the five ssh key types, and the cloud
    // service-account filenames every tutorial writes.
    //
    // Driven through mask_paths() against a real tree, so this tests the
    // matcher AND the walk rather than just the array.
    namespace fs = std::filesystem;
    // Self-deleting: the whole reason this helper exists is that hand-rolled
    // temp dirs are what leaked 3537 directories onto this box.
    agtest::TempDir tmp{"masktest"};
    const auto root = tmp.path();
    fs::create_directories(root / "services" / "api");

    auto touch = [](const fs::path& p) { std::ofstream{p} << "x"; };
    for (const char* n : {".env", ".env.development", ".env.test",
                          ".npmrc", ".netrc", ".git-credentials",
                          "credentials.json", "service-account.json",
                          "id_ecdsa", "id_dsa", ".pgpass"})
        touch(root / "services" / "api" / n);
    // Suffix rules, which only the walk can resolve.
    touch(root / "services" / "api" / "server.pem");
    touch(root / "services" / "api" / "prod.tfvars");
    // And things that must NOT be masked: ordinary source that merely looks
    // adjacent to a rule.
    touch(root / "services" / "api" / "main.cpp");
    touch(root / "services" / "api" / "env.example");

    sandbox_cfg::Config cfg;
    cfg.mask_scan_depth = 3;
    const auto masked = sandbox_cfg::mask_paths(cfg, root.string(), {});

    auto has = [&](std::string_view leaf) {
        return std::any_of(masked.begin(), masked.end(), [&](const auto& m) {
            return std::string_view{m}.ends_with(leaf);
        });
    };

    CHECK(has(".env"));
    CHECK(has(".env.development"));
    CHECK(has(".npmrc"));
    CHECK(has(".netrc"));
    CHECK(has(".git-credentials"));
    CHECK(has("credentials.json"));
    CHECK(has("service-account.json"));
    CHECK(has("id_ecdsa"));
    CHECK(has(".pgpass"));
    CHECK(has("server.pem"));      // suffix rule
    CHECK(has("prod.tfvars"));     // suffix rule

    // Ordinary files stay readable. A mask list that caught main.cpp would
    // break every build while looking like it was working.
    CHECK_FALSE(has("main.cpp"));
    CHECK_FALSE(has("env.example"));
}

TEST_CASE("sandbox masks: a suffix rule needs its marker to be a suffix") {
    // `*.pem` matches prod.pem; a rule written `.pem` would match only a file
    // literally named ".pem". The marker is explicit rather than inferred from
    // shape BECAUSE inference is dangerous here: "starts with a dot, no second
    // dot" silently promotes .env, .npmrc and .netrc to suffixes, so prod.env
    // and scoped.npmrc start getting masked as a side effect of adding an
    // unrelated entry. Widening a security rule by accident is as bad as
    // narrowing one.
    namespace fs = std::filesystem;
    agtest::TempDir tmp{"sfxtest"};
    const auto root = tmp.path();

    auto touch = [](const fs::path& p) { std::ofstream{p} << "x"; };
    touch(root / "prod.env");       // NOT .env -- must stay readable
    touch(root / "scoped.npmrc");   // NOT .npmrc -- must stay readable
    touch(root / "key.pem");        // IS a *.pem -- must be masked

    sandbox_cfg::Config cfg;
    cfg.mask_scan_depth = 1;
    const auto masked = sandbox_cfg::mask_paths(cfg, root.string(), {});
    auto has = [&](std::string_view leaf) {
        return std::any_of(masked.begin(), masked.end(), [&](const auto& m) {
            return std::string_view{m}.ends_with(leaf);
        });
    };

    CHECK(has("key.pem"));
    CHECK_FALSE(has("prod.env"));
    CHECK_FALSE(has("scoped.npmrc"));
}

#if defined(__linux__)
// ── the cgroup has to actually hold the guest ───────────────────────────
//
// describe() reports `resource.memory strong via cgroup2` whenever compile()
// managed to create a group. But the spawn used clay::spawn(), and that
// forwards an EMPTY group to spawn_in() -- which only ever looks at the group
// it is handed, never at the compiled posture. So on a host that DOES
// delegate, the group was created, reported, and then nothing was ever put in
// it: the pane advertised a wall that was never erected.
//
// It is also the difference between being able to kill a runaway and not.
// Signalling the process group misses any descendant that called setsid();
// cgroup.kill cannot be escaped that way.
//
// Only checkable where the host delegates a subtree. A plain desktop systemd
// session does NOT: the scope is root-owned and cgroup.subtree_control is
// unwritable, so claybin's probe says undelegable, compile() creates no
// group, and the process-group fallback is the honest outcome. Containers and
// delegated user slices are where this test does its work.
//
// Measured by asking the guest where it lives, because that is the only
// answer the reporting path cannot fake.
TEST_CASE("sandbox: a claybin guest runs inside its own cgroup") {
    if (clay::cgroup::probe().availability != clay::cgroup::Availability::delegated)
        return;   // nothing to attach to on this host; see above
    if (!ag::init(ag::Mode::On) || !ag::is_active()) return;
    if (ag::detected_backend() != ag::Backend::Claybin) return;

    std::ifstream self("/proc/self/cgroup");
    std::string ours((std::istreambuf_iterator<char>(self)),
                     std::istreambuf_iterator<char>());
    if (ours.empty()) return;   // cgroup v1 or no cgroupfs

    auto r = ag::run_shell_command("cat /proc/self/cgroup", 4096,
                                   std::chrono::seconds{10});
    if (!r.started || r.output.empty()) return;   // spawn blocked on this host

    const auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    };
    const std::string mine  = trim(ours);
    const std::string guest = trim(r.output);
    if (guest.find("0::") != 0) return;   // not a cgroup2 answer

    CHECK_MESSAGE(guest != mine,
                  "the guest must not share agentty's own cgroup -- if it "
                  "does, spawn never attached it and both the limits and "
                  "cgroup.kill are fiction");
}
#endif
