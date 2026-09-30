// agentty::ui::panel::Sandbox — form construction, readback, and the live
// wall preview.
//
// The form is built from a config and the HOST's capabilities, because a row
// that offers per-port network on a kernel without landlock abi 4 is a lie.
// Rows that cannot be enforced here are locked with the reason, not hidden:
// hiding them makes the limitation invisible, which is how "sandbox: active"
// came to mean nothing in issue #21.

#include "agentty/runtime/panel/sandbox.hpp"

#include <algorithm>
#include <string>

// claybin is a required submodule. The guard that remains is PLATFORM only:
// compile() is portable, so the wall preview works on any host, but the
// linux headers it needs for policy types are not.
#if defined(__linux__)
#include "claybin/plan/compile.hpp"
#include "claybin/policy/policy.hpp"
#include "claybin/policy/profiles.hpp"
#endif

namespace agentty::ui::panel {

namespace {

// Render a path list as one editable line. Comma-separated rather than a
// sub-list pane: these are usually zero or one entries, and a whole overlay
// for "sometimes two paths" is more UI than the problem deserves.
[[nodiscard]] std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += v[i];
    }
    return out;
}

[[nodiscard]] std::string join_ports(const std::vector<std::uint16_t>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(v[i]);
    }
    return out;
}

[[nodiscard]] std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t comma = s.find(',', start);
        std::string_view piece =
            s.substr(start, comma == std::string_view::npos ? s.size() - start : comma - start);
        // trim
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
            piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.remove_suffix(1);
        if (!piece.empty()) out.emplace_back(piece);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

[[nodiscard]] std::vector<std::uint16_t> split_ports(std::string_view s) {
    std::vector<std::uint16_t> out;
    for (const auto& tok : split(s)) {
        unsigned long v = std::strtoul(tok.c_str(), nullptr, 10);
        if (v > 0 && v <= 65535) out.push_back(static_cast<std::uint16_t>(v));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

form::Field header(std::string label) {
    form::Field f;
    f.label = std::move(label);
    f.value = form::field::Header{};
    return f;
}

form::Field choice(std::string_view id, std::string label, std::string help,
                   std::vector<std::string> labels, int index) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    form::field::Choice c;
    c.labels = std::move(labels);
    c.index = index;
    f.value = std::move(c);
    return f;
}

form::Field toggle(std::string_view id, std::string label, std::string help, bool on) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    f.value = form::field::Toggle{on};
    return f;
}

form::Field text(std::string_view id, std::string label, std::string help,
                 std::string value) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    form::field::Text t;
    t.value = std::move(value);
    f.value = std::move(t);
    return f;
}

// Number is an int64 with a range and no unit field -- the unit belongs in
// the label, which is also where every other pane puts it.
form::Field number(std::string_view id, std::string label, std::string help,
                   std::int64_t value, std::int64_t lo, std::int64_t hi) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    form::field::Number n;
    n.value = value;
    n.min = lo;
    n.max = hi;
    f.value = std::move(n);
    return f;
}

// Lock a row BY ID, looked up at call time.
//
// The pane's whole claim is that it tells you the truth about what is on, so
// a row offering a wall the kernel cannot build would undo that.
//
// Not by reference, and that is load-bearing rather than stylistic: the rows
// are built with push_back into a vector, so any reference taken before a
// later push is dangling. The Resources rows shipped exactly that bug -- four
// references bound, three more pushes, then all four locked, so the first
// three writes went into freed memory and the rows rendered UNLOCKED.
//
// Which is the worst possible direction for the failure to go: an unlocked
// row says "this setting applies", and the whole point of this pane is that
// it never says that when it isn't true. An id lookup cannot dangle, so the
// hazard is gone by construction instead of by careful ordering.
void lock_row(form::Form& f, std::string_view id, std::string_view reason) {
    for (auto& fld : f.fields) {
        if (fld.id != id) continue;
        fld.locked = true;
        fld.locked_reason = std::string{reason};
        return;
    }
}

}  // namespace

form::Form build_sandbox_form(const sandbox_cfg::Config& cfg, bool claybin_available,
                              std::uint32_t landlock_abi) {
    form::Form form;
    form.title = "Sandbox";

    // Which engine is being configured. FIRST row, because it decides what
    // every row below can mean: bwrap confines with mount namespaces and
    // nothing else, so the syscall profile, per-port network, W^X and the
    // cgroup caps have no bwrap spelling at all. Putting the choice at the
    // bottom would let someone tune ten rows before learning that eight of
    // them were inert.
    //
    // Both options always exist -- claybin is a required submodule, not a
    // build flag -- so this is purely a runtime choice.
    const bool on_claybin = cfg.backend == sandbox_cfg::LinuxBackend::Claybin;
    form.fields.push_back(header("Engine"));
    form.fields.push_back(choice(
        kSbBackend, "Backend",
        "bwrap: mounts only, the binary you already have. "
        "claybin: in-process, adds seccomp + landlock + cgroup2.",
        {"bwrap", "claybin"},
        static_cast<int>(cfg.backend)));
    // Asking for claybin on a host that cannot build a sandbox is worth
    // saying here rather than at spawn time. The probe actually forks and
    // attempts the uid_map write, so this is a real answer, not a guess.
    // The row stays SETTABLE -- the choice is legitimate, the host just
    // cannot honour it, and locking it would strand anyone who set it on a
    // machine where it worked.
    if (!claybin_available && on_claybin)
        form.fields.back().help =
            "claybin cannot start on this host (no user namespaces) "
            "\xc2\xb7 falling back to bwrap";

    // Every row below is enforceable only when claybin is BOTH selected and
    // usable. One predicate, used everywhere, so a row cannot disagree with
    // the engine row about what it does.
    const bool claybin_live = on_claybin && claybin_available;
    // The reason a row is locked, phrased for whichever of the two cases
    // applies -- "needs the claybin backend" is wrong and confusing when the
    // user has already selected claybin and the host is what refused.
    const std::string why_locked =
        !on_claybin ? std::string{"bwrap cannot express this \xc2\xb7 switch Backend to claybin"}
                    : std::string{"claybin cannot start on this host"};

    // The subtitle is the one-line answer to "what am I protected by right
    // now", which is the question the pane exists for.
    form.subtitle = claybin_live
                        ? std::string{"claybin \xc2\xb7 landlock abi "} +
                              std::to_string(landlock_abi) + " \xc2\xb7 seccomp \xc2\xb7 cgroup2"
                        : std::string{"bwrap \xc2\xb7 mount namespaces only"};

    // ── Filesystem ───────────────────────────────────────────────────────
    // These four are the rows bwrap CAN honour: scope, extra reads and extra
    // writes are all binds, and a mask is a bind of an empty file over the
    // path. So they stay live on both backends -- gating them would be a lie
    // in the other direction.
    //
    // Masked was locked under bwrap until the bwrap path learned to emit
    // masks; it applied none at all, so the row configured nothing. Both
    // backends honour it now (see build_bwrap_argv).
    form.fields.push_back(header("Filesystem"));
    form.fields.push_back(choice(
        kSbFsScope, "Readable scope",
        "what the command can READ. the workspace is always writable.",
        {"toolchain", "minimal", "host-readable"},
        static_cast<int>(cfg.fs_scope)));
    form.fields.push_back(
        text(kSbReadPaths, "Also readable",
             "extra paths, comma-separated. for a dependency outside the workspace.",
             join(cfg.read_paths)));
    form.fields.push_back(
        text(kSbWritePaths, "Also writable",
             "separate from readable on purpose: granting write is a different "
             "decision.",
             join(cfg.write_paths)));
    form.fields.push_back(
        text(kSbDenyPaths, "Masked",
             "carved out even inside the scope above \xc2\xb7 e.g. ~/.cargo/credentials",
             join(cfg.deny_paths)));

    // ── Network ──────────────────────────────────────────────────────────
    form.fields.push_back(header("Network"));
    form.fields.push_back(choice(
        kSbNetMode, "Access",
        "full shares the host network \xc2\xb7 none is an empty namespace \xc2\xb7 "
        "ports is per-port, enforced by landlock",
        {"full", "none", "ports"}, static_cast<int>(cfg.net_mode)));
    // Per-port needs landlock abi 4. Below that the kernel cannot express it,
    // so say so on the row rather than accepting a setting that silently
    // degrades to Full.
    if (!claybin_live)
        lock_row(form, kSbNetMode, why_locked);
    else if (landlock_abi < 4)
        lock_row(form, kSbNetMode,
                 "per-port needs landlock abi 4, host has " +
                     std::to_string(landlock_abi));

    form.fields.push_back(
        text(kSbPorts, "Allowed ports",
             "443 https \xc2\xb7 80 http \xc2\xb7 22 git-ssh \xc2\xb7 53 dns. "
             "forgetting 53 breaks everything.",
             join_ports(cfg.allow_ports)));
    // A dependent row: meaningless unless Access is `ports`. Locked for a
    // different reason than the claybin rows -- this one is about the policy
    // being incoherent, not the backend being unable to enforce it.
    if (cfg.net_mode != sandbox_cfg::NetMode::Ports)
        lock_row(form, kSbPorts, "set Access to `ports` first");

    // ── Syscalls ─────────────────────────────────────────────────────────
    form.fields.push_back(header("Syscalls"));
    form.fields.push_back(choice(
        kSbSyscalls, "Filter",
        "compiler denies ptrace/mount/unshare/bpf and filters clone flags \xc2\xb7 "
        "strict also blocks subprocesses",
        {"off", "compiler", "strict"}, static_cast<int>(cfg.syscall_mode)));
    if (!claybin_live)
        lock_row(form, kSbSyscalls, why_locked);

    form.fields.push_back(
        toggle(kSbWxProtect, "W^X",
               "no page both writable and executable. breaks JITs (some node "
               "flags, any JVM).",
               cfg.wx_protect));
    if (!claybin_live) lock_row(form, kSbWxProtect, why_locked);

    // ── Resources ─────────────────────────────────────────────────────
    // All four are cgroup2 (with rlimit as a backstop for memory/procs), and
    // agentty passes bwrap no cgroup at all. These were the rows the pane
    // shipped UNGATED -- they rendered live under bwrap and enforced nothing,
    // which is the exact failure this pane exists to prevent.
    form.fields.push_back(header("Resources"));
    form.fields.push_back(
        number(kSbMemoryMb, "Memory (MB)", "0 = no cap",
               static_cast<std::int64_t>(cfg.memory_mb), 0, 131072));
    form.fields.push_back(
        number(kSbMaxProcs, "Processes", "0 = no cap \xc2\xb7 stops fork bombs",
               static_cast<std::int64_t>(cfg.max_procs), 0, 65536));
    form.fields.push_back(
        number(kSbCpuPercent, "CPU (%)", "0 = no cap \xc2\xb7 100 = one core",
               static_cast<std::int64_t>(cfg.cpu_percent), 0, 1600));
    form.fields.push_back(
        number(kSbTmpMb, "/tmp size (MB)",
               "a runaway build hits ENOSPC inside the sandbox "
               "instead of filling host RAM",
               static_cast<std::int64_t>(cfg.tmp_mb), 16, 65536));
    if (!claybin_live)
        for (const auto id : {kSbMemoryMb, kSbMaxProcs, kSbCpuPercent, kSbTmpMb})
            lock_row(form, id, why_locked);

    // ── Hardening ─────────────────────────────────────────────────────
    form.fields.push_back(header("Hardening"));
    form.fields.push_back(toggle(
        kSbScopeIpc, "Scope IPC",
        "abstract unix sockets and cross-boundary signals. these ignore the "
        "filesystem entirely, so nothing else here covers them.",
        cfg.scope_ipc));
    if (!claybin_live)
        lock_row(form, kSbScopeIpc, why_locked);
    else if (landlock_abi < 6)
        lock_row(form, kSbScopeIpc,
                 "needs landlock abi 6, host has " + std::to_string(landlock_abi));

    // Closing inherited fds is the one hardening row that is NOT
    // claybin-only: it happens in the child before exec, so it applies
    // whichever engine builds the walls. Worth stating, because bubblewrap
    // itself leaks one and that is the reason the row exists.
    form.fields.push_back(toggle(
        kSbCloseFds, "Close inherited fds",
        "an inherited descriptor is authority the sandbox cannot revoke. "
        "bubblewrap leaks one.",
        cfg.close_inherited_fds));

    // The trust-handoff row goes LAST and under its own heading, because it is
    // not the same kind of control as the rest. Everything above confines the
    // process; this one governs what the agent may hand to the host to run
    // later -- the escape class in Pillar's July 2026 series, where the agent
    // never broke out and did not need to.
    form.fields.push_back(header("Trust handoff"));
    form.fields.push_back(choice(
        kSbHandoff, "Agent writes host-executed files",
        "hooks, .vscode tasks, git config, venv interpreters \xc2\xb7 "
        "the shape of CVE-2026-48124 \xc2\xb7 refuse is the default on purpose",
        {"refuse", "warn", "allow"}, static_cast<int>(cfg.handoff)));

    return form;
}

sandbox_cfg::Config read_sandbox_form(const form::Form& f,
                                      const sandbox_cfg::Config& base) {
    sandbox_cfg::Config cfg = base;
    cfg.configured = true;

    auto choice_of = [&](std::string_view id, int fallback) {
        if (const auto* fld = f.find(id))
            if (const auto* c = std::get_if<form::field::Choice>(&fld->value)) return c->index;
        return fallback;
    };
    auto toggle_of = [&](std::string_view id, bool fallback) {
        if (const auto* fld = f.find(id))
            if (const auto* t = std::get_if<form::field::Toggle>(&fld->value)) return t->on;
        return fallback;
    };
    auto text_of = [&](std::string_view id) -> std::string {
        if (const auto* fld = f.find(id))
            if (const auto* t = std::get_if<form::field::Text>(&fld->value)) return t->value;
        return {};
    };
    auto num_of = [&](std::string_view id, std::int64_t fallback) -> std::int64_t {
        if (const auto* fld = f.find(id))
            if (const auto* n = std::get_if<form::field::Number>(&fld->value)) return n->value;
        return fallback;
    };

    // The engine first, matching the form order. A locked row still reads
    // back its current value, so switching to bwrap and saving keeps the
    // claybin-only numbers in the config rather than zeroing them -- the
    // user's settings survive a round trip through the other backend.
    cfg.backend = static_cast<sandbox_cfg::LinuxBackend>(
        choice_of(kSbBackend, static_cast<int>(base.backend)));

    cfg.fs_scope = static_cast<sandbox_cfg::FsScope>(
        choice_of(kSbFsScope, static_cast<int>(base.fs_scope)));
    cfg.read_paths = split(text_of(kSbReadPaths));
    cfg.write_paths = split(text_of(kSbWritePaths));
    cfg.deny_paths = split(text_of(kSbDenyPaths));

    cfg.net_mode = static_cast<sandbox_cfg::NetMode>(
        choice_of(kSbNetMode, static_cast<int>(base.net_mode)));
    cfg.allow_ports = split_ports(text_of(kSbPorts));

    cfg.syscall_mode = static_cast<sandbox_cfg::SyscallMode>(
        choice_of(kSbSyscalls, static_cast<int>(base.syscall_mode)));
    cfg.wx_protect = toggle_of(kSbWxProtect, base.wx_protect);

    cfg.memory_mb = static_cast<std::uint64_t>(num_of(kSbMemoryMb, 0));
    cfg.max_procs = static_cast<std::uint32_t>(num_of(kSbMaxProcs, 0));
    cfg.cpu_percent = static_cast<std::uint32_t>(num_of(kSbCpuPercent, 0));
    cfg.tmp_mb = static_cast<std::uint64_t>(num_of(kSbTmpMb, 512));

    cfg.scope_ipc = toggle_of(kSbScopeIpc, base.scope_ipc);
    cfg.close_inherited_fds = toggle_of(kSbCloseFds, base.close_inherited_fds);
    cfg.handoff = static_cast<sandbox_cfg::HandoffPolicy>(
        choice_of(kSbHandoff, static_cast<int>(base.handoff)));
    return cfg;
}

sandbox_cfg::Config policy_from_observation(const Observation& obs,
                                            const sandbox_cfg::Config& base) {
    sandbox_cfg::Config cfg = base;
    cfg.configured = true;

    // Start from the narrowest thing that still covers what was seen. The
    // recommendation is deliberately tight: a user reviewing a diff will widen
    // something that is too strict, but will not notice something too loose.
    cfg.fs_scope = sandbox_cfg::FsScope::Minimal;

    cfg.read_paths = obs.paths_read;
    cfg.write_paths = obs.paths_written;
    std::sort(cfg.read_paths.begin(), cfg.read_paths.end());
    cfg.read_paths.erase(std::unique(cfg.read_paths.begin(), cfg.read_paths.end()),
                         cfg.read_paths.end());
    std::sort(cfg.write_paths.begin(), cfg.write_paths.end());
    cfg.write_paths.erase(std::unique(cfg.write_paths.begin(), cfg.write_paths.end()),
                          cfg.write_paths.end());

    // Network: if nothing connected, propose none -- that is the single
    // biggest win available and the one a hand-written policy never takes,
    // because nobody believes their build works without the internet until
    // they have watched it do so.
    if (obs.ports.empty()) {
        cfg.net_mode = sandbox_cfg::NetMode::None;
    } else {
        cfg.net_mode = sandbox_cfg::NetMode::Ports;
        cfg.allow_ports = obs.ports;
    }

    // Syscalls stay where the user had them: the observation records what the
    // profile WOULD have denied, which is information for the user, not a
    // licence to widen the filter automatically.
    return cfg;
}

#if defined(__linux__)

Preview preview_sandbox(const sandbox_cfg::Config& cfg) {
    using namespace ::clay;
    using namespace ::clay::literals;

    Preview out;

    auto d = Policy<Draft>{};
    // Only the axes that change the WALLS need to be faithful here; the exact
    // bind list does not move a guarantee, so the preview uses the shape
    // rather than the full read set. (The spawn path builds the real thing.)
    d = std::move(d).ro_bind("/usr", "/usr");
    d = std::move(d).proc_fs("/proc");
    d = std::move(d).dev_fs("/dev");
    d = std::move(d).tmpfs("/tmp", Bytes{cfg.tmp_mb * 1024 * 1024});

    switch (cfg.net_mode) {
        case sandbox_cfg::NetMode::Full:
            d = std::move(d).connect("", 0);
            break;
        case sandbox_cfg::NetMode::None:
            break;  // no grant at all: an empty netns
        case sandbox_cfg::NetMode::Ports:
            for (auto p : cfg.allow_ports) d = std::move(d).connect("", p);
            break;
    }

    switch (cfg.syscall_mode) {
        case sandbox_cfg::SyscallMode::Off:
            // everything(), NOT "leave it alone".
            //
            // A default-constructed SyscallPolicy is kill-by-default with no
            // rules, so omitting this is not "no filter" -- it is a filter
            // that kills the guest at execve, and claybin refuses to compile
            // it: "empty allow-list would kill on exec".
            //
            // The preview caught this the first time it ran. The row LOOKED
            // like a sensible off switch and would have produced a sandbox
            // that killed every command. That is the argument for having a
            // live preview at all -- the mistake is invisible in the settings
            // UI and obvious the moment you compile the policy.
            d = std::move(d).syscall_profile(SyscallPolicy::everything());
            break;
        case sandbox_cfg::SyscallMode::Compiler:
            d = std::move(d).syscall_profile(profiles::compiler_with_network());
            break;
        case sandbox_cfg::SyscallMode::Strict:
            d = std::move(d).syscall_profile(profiles::with_filesystem());
            break;
    }

    if (cfg.memory_mb) d = std::move(d).memory(Bytes{cfg.memory_mb * 1024 * 1024});
    if (cfg.max_procs) d = std::move(d).processes(cfg.max_procs);
    if (cfg.cpu_percent) d = std::move(d).cpu_percent(cfg.cpu_percent);

    d = std::move(d).new_session();
    d = std::move(d).die_with_parent();

    auto compiled = compile(std::move(d).seal(), probe_host());
    if (!compiled) {
        out.compiled = false;
        out.error = std::string{compiled.error().mechanism};
        return out;
    }
    out.compiled = true;
    out.plan_ops = compiled->plan.op_count();
    for (int i = 0; i < static_cast<int>(CapId::count_); ++i) {
        auto id = static_cast<CapId>(i);
        Wall w;
        w.name = cap_name(id);
        auto s = compiled->guarantees.strength(id);
        w.strength = s == Enforcement::strong    ? "strong"
                     : s == Enforcement::partial ? "partial"
                                                 : "none";
        w.mechanism = compiled->guarantees.mechanism(id);
        out.walls.push_back(std::move(w));
    }

    // Under BWRAP the report above describes a wall claybin would build and
    // bwrap will not. Saying "filesystem.write: strong via landlock abi 10"
    // while the command actually runs under a bwrap bind is precisely the
    // lie this pane exists to prevent -- a stronger lie than saying nothing,
    // because it comes with a receipt.
    //
    // So the compile still runs (it is how we know the policy is coherent at
    // all, and the error above is worth having either way), but the walls it
    // predicts are replaced by what bwrap actually gives: mount namespaces,
    // and nothing else.
    if (cfg.backend != sandbox_cfg::LinuxBackend::Claybin) {
        for (auto& w : out.walls) {
            // The three capabilities a bwrap bind genuinely delivers. Every
            // other CapId is seccomp, landlock or cgroup2, none of which
            // agentty hands bwrap.
            const bool via_mounts =
                w.name.starts_with("filesystem") || w.name.starts_with("mount");
            if (via_mounts) {
                w.strength  = "partial";
                w.mechanism = "bwrap bind mounts";
            } else {
                w.strength  = "none";
                w.mechanism = "bwrap cannot express this";
            }
        }
        out.unenforceable.push_back(
            "most of this policy needs the claybin backend \xc2\xb7 "
            "switch Backend above");
    }
    return out;
}

#else

Preview preview_sandbox(const sandbox_cfg::Config&) {
    // Not a missing LIBRARY -- claybin is a required submodule and always
    // linked. It is a missing KERNEL: compiling a plan needs the linux policy
    // headers, and applying one needs namespaces, landlock, seccomp and
    // cgroup2. agentty's sandbox on macOS is sandbox-exec and on Windows is
    // nothing, neither of which this pane configures.
    Preview out;
    out.compiled = false;
    out.error = "the configurable sandbox is Linux-only on this build";
    return out;
}

#endif

}  // namespace agentty::ui::panel
