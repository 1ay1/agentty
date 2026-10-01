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

EngineStatus engine_status(const sandbox_cfg::Config& cfg, const HostFacts& facts) {
    const bool wants_claybin = cfg.backend == sandbox_cfg::LinuxBackend::Claybin;
    // Host refusal outranks the comparison: if claybin cannot start here,
    // "applies on restart" would be a promise nothing can keep.
    if (wants_claybin && !facts.claybin_available) return EngineStatus::CannotStart;
    if (!facts.sandbox_active) return EngineStatus::AppliesOnRestart;
    return cfg.backend == facts.running ? EngineStatus::InForce
                                        : EngineStatus::AppliesOnRestart;
}

std::string restart_outcome(const sandbox_cfg::Config& cfg,
                            const HostFacts& facts,
                            const Preview& preview) {
    // Ordered worst-first, because the footer shows ONE line and the user
    // needs the thing that most invalidates the promise. A policy that will
    // not compile makes the unenforceable list irrelevant; an engine that
    // cannot start makes the compile result irrelevant.

    // 1. It does not compile. claybin refuses rather than degrading, so there
    //    is no "partly" here -- the next launch gets no policy from this.
    if (!preview.compiled)
        return "will NOT apply on restart \xc2\xb7 " + preview.error;

    // 2. The engine cannot start here. Saving is still legitimate (the user
    //    may be on a different machine tomorrow, and the config is portable)
    //    but the promise has to name the fallback rather than imply the walls
    //    below will be up.
    if (engine_status(cfg, facts) == EngineStatus::CannotStart)
        return "claybin cannot start on this host \xc2\xb7 restart falls back to "
               "bwrap, which enforces only the mount walls";

    // 3. It compiles and starts, but something comes out weaker than asked.
    //    This is the quiet case and the reason the function exists: the rows
    //    describe a boundary, the restart delivers most of it, and without
    //    saying so the user believes in the part that silently degraded.
    if (!preview.unenforceable.empty()) {
        std::string out = "applies on restart, but this host cannot enforce: ";
        for (std::size_t i = 0; i < preview.unenforceable.size(); ++i) {
            if (i) out += ", ";
            out += preview.unenforceable[i];
        }
        return out;
    }

    // Empty: the restart really will deliver the policy as described, and the
    // footer may make its promise plainly.
    return {};
}

std::string describe_running(const HostFacts& facts) {
    // Reads ONLY `facts`. The selected engine is deliberately not a parameter:
    // this function answers "what is confining commands right now", and the
    // bug it exists to prevent was exactly that question being answered from
    // the row the user had just moved.
    if (!facts.sandbox_active)
        return "no sandbox \xc2\xb7 commands run unconfined";
    if (facts.running == sandbox_cfg::LinuxBackend::Claybin)
        return "claybin \xc2\xb7 landlock abi " + std::to_string(facts.landlock_abi) +
               " \xc2\xb7 seccomp \xc2\xb7 cgroup2";
    return "bwrap \xc2\xb7 mount namespaces only";
}

form::Form build_sandbox_form(const sandbox_cfg::Config& cfg,
                              const HostFacts& facts) {
    const bool claybin_available = facts.claybin_available;
    const std::uint32_t landlock_abi = facts.landlock_abi;
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
    //
    // NOTE this is about what a SAVE would enforce, not about what is running.
    // Those are different questions and conflating them is what made the
    // subtitle lie -- see HostFacts. A row locked here is one that would do
    // nothing even after a restart; a row that is merely not-yet-in-force is
    // handled by the status line, not by locking.
    const bool claybin_live = on_claybin && claybin_available;
    // The reason a row is locked, phrased for whichever of the two cases
    // applies -- "needs the claybin backend" is wrong and confusing when the
    // user has already selected claybin and the host is what refused.
    const std::string why_locked =
        !on_claybin ? std::string{"bwrap cannot express this \xc2\xb7 switch Backend to claybin"}
                    : std::string{"claybin cannot start on this host"};

    // The subtitle is the one-line answer to "what am I protected by RIGHT
    // NOW", which is the question the pane exists for -- and the question it
    // got wrong.
    //
    // It is built from `facts` alone and never from the selected engine. The
    // old version used `on_claybin && claybin_available`, so moving the row to
    // claybin instantly repainted this to claim seccomp + landlock + cgroup2
    // on a process still running bwrap, which cannot change mid-session
    // because the policy is sealed at startup. That is the "sandbox: active
    // while nothing is enforced" failure this entire pane was built after.
    //
    // When the selection and reality disagree, the disagreement is stated
    // here rather than resolved in favour of either one: the user needs both
    // facts, and which is which.
    form.subtitle = describe_running(facts);
    switch (engine_status(cfg, facts)) {
        case EngineStatus::InForce:
            break;   // selection matches reality; nothing to disclaim
        case EngineStatus::AppliesOnRestart:
            form.subtitle += "  \xc2\xb7  selected: ";
            form.subtitle += sandbox_cfg::to_string(cfg.backend);
            form.subtitle += " (applies on restart)";
            break;
        case EngineStatus::CannotStart:
            form.subtitle += "  \xc2\xb7  claybin cannot start here";
            break;
    }

    // ── Posture ───────────────────────────────────────────────────
    //
    // Second row, directly under the engine, because it is the question the
    // user actually has. Twenty-eight individually-correct switches is still
    // the wrong thing to hand someone who wants "tighter than this" -- and a
    // control nobody can reason about is a control nobody touches, which for a
    // security setting means it is off.
    //
    // It does not REPLACE the rows, it writes them. Everything below stays
    // visible and editable, so this is a starting point rather than a mode:
    // the moment a preset hid the detail it would be another "sandbox: active"
    // — a label standing in for a boundary you can no longer inspect.
    //
    // `Custom` is in the list but is never a thing you pick: it is what the
    // row REPORTS when the config matches no preset, so editing one row off
    // Hardened stops the pane claiming Hardened. Selecting it is a no-op by
    // construction (apply_posture returns the config untouched).
    const auto posture = sandbox_cfg::detect_posture(cfg);
    form.fields.push_back(choice(
        kSbPosture, "Posture",
        // The COST, not the benefit. All four names sound safe, so the thing a
        // user needs before choosing is what will stop working.
        sandbox_cfg::cost_of(posture),
        {"permissive", "balanced", "hardened", "airgapped", "custom"},
        static_cast<int>(posture)));

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
    form.fields.push_back(
        number(kSbOpenFiles, "Open files",
               "0 = no cap \xc2\xb7 a leak exhausts the host's file table "
               "without ever forking, so Processes does not bound it",
               static_cast<std::int64_t>(cfg.max_open_files), 0, 1048576));
    form.fields.push_back(
        number(kSbCpuSecs, "CPU seconds",
               "0 = no cap \xc2\xb7 total compute, so a process that sleeps "
               "forever is untouched",
               static_cast<std::int64_t>(cfg.cpu_secs), 0, 86400));
    form.fields.push_back(
        number(kSbWallClock, "Wall clock (s)",
               "0 = no cap \xc2\xb7 elapsed time, enforced by the sandbox \xc2\xb7 "
               "still applies to a child that ignores SIGTERM",
               static_cast<std::int64_t>(cfg.wall_clock_secs), 0, 86400));
    if (!claybin_live)
        for (const auto id : {kSbMemoryMb, kSbMaxProcs, kSbCpuPercent, kSbTmpMb,
                              kSbOpenFiles, kSbCpuSecs, kSbWallClock})
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

    // Hostname. NOT a containment control, and the help says so rather than
    // letting it sit among the walls looking like one -- the guest cannot
    // escalate either way. It is here because the real host name leaks into
    // build output and test snapshots, which makes those non-reproducible.
    form.fields.push_back(toggle(
        kSbFakeHost, "Report hostname as `sandbox`",
        "not a wall \xc2\xb7 keeps the real host name out of build output and "
        "test snapshots",
        cfg.fake_hostname));
    if (!claybin_live) lock_row(form, kSbFakeHost, why_locked);

    // How deep to hunt for credential FILES by name (.env, id_rsa, *.pem).
    // Applies to both backends, because the sweep produces a path list and
    // both know how to mask a path.
    //
    // A cost row, unusually: every level is more stat() calls on every spawn,
    // and the sweep runs in the latency path of each shell command. Saying so
    // is better than picking a number and hiding the trade.
    form.fields.push_back(
        number(kSbMaskDepth, "Secret scan depth",
               "0 = workspace root only \xc2\xb7 3 finds services/*/.env \xc2\xb7 "
               "each level costs stat() calls on every command",
               static_cast<std::int64_t>(cfg.mask_scan_depth), 0, 8));

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
    cfg.max_open_files = static_cast<std::uint32_t>(num_of(kSbOpenFiles, 0));
    cfg.cpu_secs = static_cast<std::uint32_t>(num_of(kSbCpuSecs, 0));
    cfg.wall_clock_secs = static_cast<std::uint32_t>(num_of(kSbWallClock, 0));
    cfg.mask_scan_depth = static_cast<std::uint32_t>(
        num_of(kSbMaskDepth, static_cast<std::int64_t>(base.mask_scan_depth)));
    cfg.fake_hostname = toggle_of(kSbFakeHost, base.fake_hostname);

    cfg.scope_ipc = toggle_of(kSbScopeIpc, base.scope_ipc);
    cfg.close_inherited_fds = toggle_of(kSbCloseFds, base.close_inherited_fds);
    cfg.handoff = static_cast<sandbox_cfg::HandoffPolicy>(
        choice_of(kSbHandoff, static_cast<int>(base.handoff)));

    // The POSTURE row is deliberately NOT read back here.
    //
    // It cannot be, soundly. Readback sees only values, and the posture row's
    // value is the posture the config USED to be -- so after the user picks
    // Hardened and then edits Memory, the row still reads "hardened" while the
    // config is genuinely Custom. Any rule inferring intent from that pair gets
    // it wrong in one direction or the other: compare-and-apply re-stamps 8192
    // over the user's 2048 on the next repaint, and apply-always is worse.
    //
    // Applying a preset is an ACTION, and only the reducer knows an action
    // happened -- it has the row id that changed. So `apply_posture` is called
    // there (update/sandbox.cpp, the kSbPosture arm) exactly once per keystroke
    // that moves this row, and readback stays a pure projection of the other
    // rows. The row's displayed value comes from `detect_posture` at build time,
    // which is derived and therefore cannot drift.
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

void annotate_sandbox_form(form::Form& f, const Preview& preview,
                          const sandbox_cfg::Config& cfg, EngineStatus status) {
    // Walls described while the selected engine is not the running one are a
    // FORECAST, and every annotation below has to say so.
    //
    // Without this the per-row honesty added in §15 becomes the same lie the
    // subtitle told: "strong via seccomp-bpf" next to a row, in the present
    // tense, on a process whose seccomp filter does not exist. The suffix is
    // short because it goes on every annotated row -- and it goes on every row
    // rather than once in the footer precisely because the footer is what
    // people skip (§15).
    const char* tense = status == EngineStatus::InForce ? "" : " (on restart)";
    // Which capability answers for which row. The mapping is the whole point of
    // the function and the only interesting thing in it: a row the user edits
    // is a POLICY axis, and the wall report is indexed by CAPABILITY, so
    // without this table the honest answer sits one translation away from the
    // question. claybin's cap_name() strings are the keys (see
    // claybin/core/witness.hpp).
    //
    // Not every row has one, and the gaps are honest rather than missing:
    // `Also readable` is a bind list, not a capability -- the wall that governs
    // it is filesystem.read, already shown on the scope row above it, and
    // repeating it on four path rows would be noise pretending to be rigour.
    struct RowCap { std::string_view row; std::string_view cap; };
    static constexpr RowCap kRowCaps[] = {
        {kSbFsScope,    "filesystem.read"},
        {kSbNetMode,    "network.isolation"},
        {kSbSyscalls,   "syscall.filter"},
        {kSbMemoryMb,   "resource.memory"},
        {kSbMaxProcs,   "resource.pids"},
        {kSbCpuPercent, "resource.cpu"},
    };

    auto row_of = [&](std::string_view id) -> form::Field* {
        for (auto& fld : f.fields)
            if (fld.id == id) return &fld;
        return nullptr;
    };
    auto wall_of = [&](std::string_view cap) -> const Wall* {
        for (const auto& w : preview.walls)
            if (w.name == cap) return &w;
        return nullptr;
    };

    // ── the walls, next to the rows that ask for them ────────────────────
    for (const auto& [row_id, cap] : kRowCaps) {
        auto* fld = row_of(row_id);
        if (!fld) continue;

        if (!preview.compiled) {
            // The policy does not compile, so NO row's wall is known. Saying
            // "strong" here off a stale preview would be the worst possible
            // lie: confident, per-row, and wrong.
            fld->origin = "\xe2\x80\x94";
            continue;
        }
        const auto* w = wall_of(cap);
        if (!w) continue;

        // "strong via landlock abi 10", not "strong". The mechanism IS the
        // receipt -- a strength on its own is the same unfalsifiable claim as
        // "sandbox: active", which is the bug this pane was built after.
        fld->origin = (w->mechanism.empty()
            ? w->strength
            : w->strength + " via " + w->mechanism) + tense;
    }

    // The posture row gets the SCORE instead of a mechanism: it is not one
    // capability, it is the whole set. "9/11 strong" turns the preset from a
    // name you have to trust into a number you can compare, and it is the one
    // annotation that makes two postures orderable at a glance.
    //
    // Kernel isolation is excluded from the denominator rather than counted as
    // a failure, because no process backend can ever satisfy it (§13). Leaving
    // it in would cap every posture below 100% for a reason that has nothing
    // to do with the user's choice.
    if (auto* fld = row_of(kSbPosture)) {
        if (!preview.compiled) {
            fld->origin = "will not compile";
        } else {
            std::size_t strong = 0, total = 0;
            for (const auto& w : preview.walls) {
                if (w.name == "host.kernel_isolation") continue;
                ++total;
                if (w.strength == "strong") ++strong;
            }
            if (total)
                fld->origin = std::to_string(strong) + "/" +
                              std::to_string(total) + " strong" + tense;
        }
    }

    // ── validation: rows that defeat themselves ──────────────────────────
    //
    // Each of these is a setting that COMPILES and does not do what it reads
    // like. That is the dangerous class: a config error claybin would reject is
    // already reported in the footer, but a config that quietly means something
    // else has no other surface to appear on.

    // Per-port network with no ports is an empty namespace: the user asked to
    // allow a specific set and allowed nothing.
    if (cfg.net_mode == sandbox_cfg::NetMode::Ports && cfg.allow_ports.empty()) {
        if (auto* fld = row_of(kSbPorts))
            fld->error = "no ports listed, so this denies all network \xc2\xb7 "
                         "set Access to none if that is what you meant";
    }

    // Host-readable is not a scope, it is the absence of one. Not an error --
    // it is a legitimate choice for a user who wants only the write wall -- but
    // it must not read like one more option on a list.
    if (cfg.fs_scope == sandbox_cfg::FsScope::HostReadable) {
        if (auto* fld = row_of(kSbFsScope))
            fld->error = "grants read on / \xc2\xb7 secrets outside the mask list "
                         "are reachable";
    }

    // Off is a real choice (a user whose toolchain breaks under the filter
    // should be able to say so) but it also disables BROKERING, because the
    // broker is the filter deciding at runtime. Turning off one row silently
    // turning off another is exactly the kind of coupling a user cannot infer.
    if (cfg.syscall_mode == sandbox_cfg::SyscallMode::Off) {
        if (auto* fld = row_of(kSbSyscalls))
            fld->error = "no filter, and no brokering either \xc2\xb7 "
                         "ptrace and kill stop being supervised";
    }

    // A handoff policy other than refuse is the one setting here that widens
    // the blast radius rather than narrowing it, and it does so OUTSIDE the
    // sandbox where no wall reports on it. The footer's wall report structurally
    // cannot mention it, so the row has to carry its own warning.
    if (cfg.handoff != sandbox_cfg::HandoffPolicy::Refuse) {
        if (auto* fld = row_of(kSbHandoff))
            fld->error = cfg.handoff == sandbox_cfg::HandoffPolicy::Allow
                ? "the agent may author files your host later executes \xc2\xb7 "
                  "this is the shape of every escape in Pillar's 2026 series"
                : "allowed and recorded \xc2\xb7 the write still happens";
    }

    // Secret masking off entirely. Depth 0 still covers the workspace root, so
    // this is not "no masking" -- saying which is better than letting the user
    // guess from a 0.
    if (cfg.mask_scan_depth == 0) {
        if (auto* fld = row_of(kSbMaskDepth))
            fld->error = "root only \xc2\xb7 a nested services/*/.env stays readable";
    }
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

void annotate_sandbox_form(form::Form&, const Preview&,
                           const sandbox_cfg::Config&, EngineStatus) {
    // Nothing to annotate: there are no walls to report and every row is
    // already locked by build_sandbox_form on a host with no backend. A
    // per-row "none" next to a row that already says why would be the same
    // fact twice.
}

#endif

}  // namespace agentty::ui::panel
