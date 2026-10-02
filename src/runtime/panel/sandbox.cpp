// agentty::ui::panel::Sandbox — form construction, readback, and the live
// wall preview.
//
// The form is built from a config and the HOST's capabilities, because a row
// that offers per-port network on a kernel without landlock abi 4 is a lie.
// Rows that cannot be enforced here are locked with the reason, not hidden:
// hiding them makes the limitation invisible, which is how "sandbox: active"
// came to mean nothing in issue #21.

#include "agentty/runtime/panel/sandbox.hpp"
#include "agentty/runtime/panel/mention.hpp"   // file_source/file_filter, shared with @
#include "agentty/i18n/i18n.hpp"              // t() — row labels and help

#include <algorithm>
#include <string>
#include <unordered_set>   // directory dedup in path_source()

// claybin is a required submodule. The guard that remains is PLATFORM only:
// compile() is portable, so the wall preview works on any host.
//
// POSIX rather than Linux now that macOS applies claybin policies too. The
// rows this guard controls -- the syscall profile, per-port network, the
// resource caps, the extra path grants -- are the SETTINGS, and they are only
// meaningful under claybin whichever platform is underneath. Leaving it at
// Linux meant that on a mac the pane silently dropped every one of them: the
// policy still compiled and was still enforced, but the user had no row to
// see or change it with.
#if defined(__linux__) || defined(__APPLE__)
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

// A pick-one row.
//
// `hints` is per-OPTION and is the difference between a row you can operate
// and a row you have to already understand. The help line explains what the
// row is; the hint explains what the option you are looking at DOES, as you
// cycle onto it. Every other form pane in agentty passes these and this one
// did not, which is most of why its enum rows read as jargon: "toolchain /
// minimal / host-readable" names three things without saying what any of them
// grants.
form::Field choice(std::string_view id, std::string label, std::string help,
                   std::vector<std::string> labels, int index,
                   std::vector<std::string> hints = {}) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    form::field::Choice c;
    c.labels = std::move(labels);
    c.hints = std::move(hints);
    c.index = index;
    f.value = std::move(c);
    return f;
}

// A row that OPENS a list editor instead of holding a comma-separated string.
//
// Rendered as the entry count rather than the contents: the row's job is to
// say whether the list is empty and get you into it, and a truncated preview of
// three paths in a narrow column tells you less than "3 paths" does while
// looking like it is telling you more.
form::Field list_row(std::string_view id, std::string label, std::string help,
                     std::size_t count, const char* noun) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    f.help = std::move(help);
    form::field::Pick p;
    p.label = count == 0
        ? std::string{}
        : std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
    p.placeholder = "none \xc2\xb7 Enter to add";
    f.value = std::move(p);
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

// A boolean row.
//
// `when_on` / `when_off` is the toggle's answer to a Choice's per-option
// hints: the help line describes what the CURRENT state does, and flips when
// you flip it. A toggle renders as nothing but on/off, so without this the
// only way to learn what turning it off costs you is to turn it off and find
// out -- which on a security control is the wrong way round.
//
// Phrased as consequence, not restatement. "off: no filter" teaches nothing;
// "off: a JIT works, and a bug can rewrite its own code" is the actual trade.
form::Field toggle(std::string_view id, std::string label, std::string help,
                   bool on, std::string when_on = {}, std::string when_off = {}) {
    form::Field f;
    f.id = std::string{id};
    f.label = std::move(label);
    const auto& state = on ? when_on : when_off;
    f.help = state.empty()
        ? std::move(help)
        : (help.empty() ? state : help + " \xc2\xb7 " + state);
    form::field::Toggle t;
    t.on = on;
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
    if (facts.mode_off)
        return "sandboxing is OFF (--sandbox off) \xc2\xb7 nothing below is enforced";
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
        "which engine builds the walls. claybin is stronger; bwrap is the "
        "fallback for hosts it cannot run on.",
        {"bwrap", "claybin"},
        static_cast<int>(cfg.backend),
        {"mount namespaces only \xc2\xb7 filesystem and masks work; the syscall "
         "filter, resource limits and per-port network do NOT",
         "every row below works \xc2\xb7 seccomp + landlock + cgroup2, needs "
         "unprivileged user namespaces"}));
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
    form.fields.push_back(header("Posture \xe2\x80\x94 a preset for every row below"));
    form.fields.push_back(choice(
        kSbPosture, "Preset",
        // Say what the control IS before what it costs. "Posture" was a word
        // the pane used without ever defining, and a settings row whose label
        // you have to already understand is a row people skip.
        posture == sandbox_cfg::Posture::Custom
            ? std::string{"your own mix \xc2\xb7 pick one to overwrite every row below"}
            : std::string{"sets every row below in one go \xc2\xb7 "} +
                  sandbox_cfg::cost_of(posture),
        {"permissive", "balanced", "hardened", "airgapped", "custom"},
        static_cast<int>(posture),
        {"mount walls only \xc2\xb7 no syscall filter, no limits \xc2\xb7 for a "
         "toolchain that breaks under one",
         "the default \xc2\xb7 network on, compiler syscall profile, fork-bomb cap",
         "every wall claybin can build \xc2\xb7 limits a real build still survives",
         "hardened + no network \xc2\xb7 nothing can send your code anywhere",
         "not a preset \xc2\xb7 what the pane shows once you edit a row yourself"}));

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
        "what a command can READ. the workspace is always readable and "
        "writable; this is everything else.",
        {"toolchain", "minimal", "host-readable"},
        static_cast<int>(cfg.fs_scope),
        {"/usr, /bin, /lib, /etc + ~/.cargo, ~/.npm and friends \xc2\xb7 what a "
         "build needs",
         "system dirs only \xc2\xb7 no $HOME toolchains, so some builds break",
         "read on / \xc2\xb7 every secret outside the mask list is reachable"}));
    form.fields.push_back(
        list_row(kSbReadPaths, std::string{i18n::t("sandbox.read_paths")},
                 std::string{i18n::t("sandbox.help.read_paths")},
                 cfg.read_paths.size(), "path"));
    form.fields.push_back(
        list_row(kSbWritePaths, std::string{i18n::t("sandbox.write_paths")},
                 std::string{i18n::t("sandbox.help.write_paths")},
                 cfg.write_paths.size(), "path"));
    form.fields.push_back(
        list_row(kSbDenyPaths, std::string{i18n::t("sandbox.deny_paths")},
                 std::string{i18n::t("sandbox.help.deny_paths")},
                 cfg.deny_paths.size(), "path"));

    // ── Network ──────────────────────────────────────────────────────────
    form.fields.push_back(header("Network"));
    form.fields.push_back(choice(
        kSbNetMode, "Access",
        "whether a command can reach the network at all.",
        {"full", "none", "ports"}, static_cast<int>(cfg.net_mode),
        {"shares the host network \xc2\xb7 git push and npm install work, and so "
         "does exfiltration",
         "an empty network namespace \xc2\xb7 nothing can connect out",
         "only the ports listed below \xc2\xb7 needs landlock abi 4+"}));
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
        list_row(kSbPorts, std::string{i18n::t("sandbox.ports")},
                 std::string{i18n::t("sandbox.help.ports")},
                 cfg.allow_ports.size(), "port"));
    // Deliberately NOT locked when Access is not `ports`.
    //
    // It used to be, on the reasoning that a port list means nothing under
    // `full` or `none`. True, and it made the row unreachable: lock_row makes
    // activate() return Nothing, so Enter did not open the editor and there
    // was no way to fill in ports at all. The user had to know to set Access
    // first, from a row that told them to do it but would not let them act.
    //
    // So the row stays live and the EDITOR fixes the mode on commit: adding
    // ports when Access is not `ports` switches it, because adding a port is
    // an unambiguous statement of intent. The help line says so rather than
    // leaving it to be discovered.
    if (cfg.net_mode != sandbox_cfg::NetMode::Ports)
        form.fields.back().help =
            "adding a port here switches Access to `ports` \xc2\xb7 "
            "443 https \xc2\xb7 80 http \xc2\xb7 22 git-ssh \xc2\xb7 53 dns";

    // ── Syscalls ─────────────────────────────────────────────────────────
    form.fields.push_back(header("Syscalls"));
    form.fields.push_back(choice(
        kSbSyscalls, "Filter",
        "which syscalls a command may make. the filter also decides ptrace and "
        "kill at runtime rather than killing the process.",
        {"off", "compiler", "strict"}, static_cast<int>(cfg.syscall_mode),
        {"no filter, and no ptrace/kill supervision either",
         "denies ptrace, mount, unshare, bpf and filters clone flags \xc2\xb7 "
         "what a compiler needs",
         "compiler, plus no subprocesses at all"}));
    if (!claybin_live)
        lock_row(form, kSbSyscalls, why_locked);

    form.fields.push_back(
        toggle(kSbWxProtect, "W^X",
               "no page may be both writable and executable",
               cfg.wx_protect,
               "on: a bug that writes code cannot then run it \xc2\xb7 "
               "breaks JITs (some node flags, any JVM)",
               "off: JITs work \xc2\xb7 a memory bug can write new code and "
               "execute it"));
    if (!claybin_live) lock_row(form, kSbWxProtect, why_locked);

    // ── Resources ─────────────────────────────────────────────────────
    // All four are cgroup2 (with rlimit as a backstop for memory/procs), and
    // agentty passes bwrap no cgroup at all. These were the rows the pane
    // shipped UNGATED -- they rendered live under bwrap and enforced nothing,
    // which is the exact failure this pane exists to prevent.
    form.fields.push_back(header("Resources"));
    form.fields.push_back(
        number(kSbMemoryMb, std::string{i18n::t("sandbox.memory")},
               std::string{i18n::t("sandbox.help.memory")},
               static_cast<std::int64_t>(cfg.memory_mb), 0, 131072));
    form.fields.push_back(
        number(kSbMaxProcs, std::string{i18n::t("sandbox.procs")},
               std::string{i18n::t("sandbox.help.procs")},
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
        "abstract unix sockets and cross-boundary signals \xc2\xb7 these ignore "
        "the filesystem entirely, so nothing else here covers them",
        cfg.scope_ipc,
        "on: the command cannot talk to host processes over an abstract "
        "socket or signal them",
        "off: it can reach any listening host process, sandbox or not"));
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
        "an inherited descriptor is authority the sandbox cannot revoke \xc2\xb7 "
        "bubblewrap leaks one",
        cfg.close_inherited_fds,
        "on: the command starts with only stdin/stdout/stderr",
        "off: it inherits agentty's open files \xc2\xb7 a handle to a masked "
        "file still reads it"));

    // Hostname. NOT a containment control, and the help says so rather than
    // letting it sit among the walls looking like one -- the guest cannot
    // escalate either way. It is here because the real host name leaks into
    // build output and test snapshots, which makes those non-reproducible.
    form.fields.push_back(toggle(
        kSbFakeHost, "Report hostname as `sandbox`",
        "not a wall \xc2\xb7 a privacy and reproducibility setting",
        cfg.fake_hostname,
        "on: builds and test snapshots see `sandbox` instead of your "
        "machine name",
        "off: the real host name appears in build output and any snapshot "
        "that records it"));
    if (!claybin_live) lock_row(form, kSbFakeHost, why_locked);

    // How deep to hunt for credential FILES by name (.env, id_rsa, *.pem).
    // Applies to both backends, because the sweep produces a path list and
    // both know how to mask a path.
    //
    // A cost row, unusually: every level is more stat() calls on every spawn,
    // and the sweep runs in the latency path of each shell command. Saying so
    // is better than picking a number and hiding the trade.
    form.fields.push_back(
        number(kSbMaskDepth, std::string{i18n::t("sandbox.scan_depth")},
               std::string{i18n::t("sandbox.help.scan_depth")},
               static_cast<std::int64_t>(cfg.mask_scan_depth), 0, 8));

    // The trust-handoff row goes LAST and under its own heading, because it is
    // not the same kind of control as the rest. Everything above confines the
    // process; this one governs what the agent may hand to the host to run
    // later -- the escape class in Pillar's July 2026 series, where the agent
    // never broke out and did not need to.
    form.fields.push_back(header("Trust handoff"));
    form.fields.push_back(choice(
        kSbHandoff, "Agent writes host-executed files",
        "hooks, .vscode tasks, git config, venv interpreters. the sandbox "
        "cannot see this \xe2\x80\x94 the file runs later, outside it.",
        {"refuse", "warn", "allow"}, static_cast<int>(cfg.handoff),
        {"the write fails with a message telling the agent what to do instead",
         "the write happens and is recorded in the feed below",
         "no gate \xc2\xb7 the shape every escape in Pillar's 2026 series used"}));

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

    // The path and port LISTS are not read back from the form.
    //
    // Those rows are Picks now -- they display a count and open an editor, so
    // the form holds no list data to read. The editor writes straight into the
    // config when it commits (SandboxListClose), and `base` carries the result
    // here. Parsing the Pick's label back into paths would be reading the UI's
    // own summary as if it were the model.
    cfg.read_paths  = base.read_paths;
    cfg.write_paths = base.write_paths;
    cfg.deny_paths  = base.deny_paths;
    cfg.allow_ports = base.allow_ports;

    cfg.net_mode = static_cast<sandbox_cfg::NetMode>(
        choice_of(kSbNetMode, static_cast<int>(base.net_mode)));

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

// Candidates are workspace files PLUS their directories.
//
// Files come from the same source the `@` picker uses, so a path that
// completes is one that exists. Directories are derived from them rather than
// walked separately: every parent prefix of a known file is a real directory,
// which costs one pass over a list we already have and cannot go stale
// independently of it.
//
// Directories matter MORE than files here. "Also readable" and "Masked" are
// almost always pointed at a tree (`/opt/weird-sdk`, `node_modules`), and a
// completer that only offered files made the common case the one you had to
// type by hand.
[[nodiscard]] ui::SnapshotSource<std::string> path_source() {
    return {
        .ready = [] { return agentty::files_ready(); },
        .fetch = [] {
            auto files = agentty::list_workspace_files();
            std::vector<std::string> all;
            all.reserve(files->size() * 2);
            // Keyed by VALUE, not by view into `all`. The views would dangle
            // the first time push_back reallocates, and a set holding
            // freed-then-reused bytes silently drops real directories rather
            // than crashing -- the worst kind of wrong.
            std::unordered_set<std::string> seen_dirs;
            for (const auto& f : *files) {
                all.push_back(f);
                // Every parent prefix, deduped. A trailing '/' marks it as a
                // directory in the list, which is also the hint the user needs
                // to tell `src` the folder from `src` the file.
                for (std::size_t i = 0; i < f.size(); ++i) {
                    if (f[i] != '/') continue;
                    std::string dir = f.substr(0, i + 1);   // keeps the '/'
                    if (seen_dirs.insert(dir).second)
                        all.push_back(std::move(dir));
                }
            }
            return util::Snapshot<std::vector<std::string>>{
                std::make_shared<const std::vector<std::string>>(std::move(all))};
        },
    };
}

SandboxListPane::SandboxListPane()
    : complete(path_source(), agentty::mention::file_filter()) {}

form::Field list_entry_text(std::string_view id, std::string label,
                            std::string help) {
    return text(id, std::move(label), std::move(help), {});
}

form::Field list_entry_number(std::string_view id, std::string label,
                              std::string help) {
    // A TEXT row, not a Number row, and the difference is the caret.
    //
    // maya::panel::Number renders a bare integer with no caret field at all --
    // it structurally cannot show one. That is right for a settings row you
    // nudge with arrows (Memory, CPU%), and wrong for a LIST entry you type
    // into: there was no way to see where you were, or that the row was even
    // live. "I don't see a caret in the port list" is exactly that.
    //
    // Digits are enforced on readback instead (read_sandbox_list parses and
    // drops anything that is not a port), so the constraint survives without
    // costing the caret.
    return text(id, std::move(label), std::move(help), {});
}

void renumber_sandbox_list(SandboxListPane& p) {
    // The LAST row is the add affordance and keeps its "+"; everything above
    // it is a real entry and gets its position. Driven off field order rather
    // than off a stored index, so an append cannot leave the labels lying.
    //
    // Writes only what CHANGED. This runs on the keystroke that fills the
    // trailing row, and unconditionally reassigning two std::strings per row
    // meant every append allocated across the whole list -- which is felt as
    // typing lag exactly when the list is long enough to need renumbering.
    // The comparison is a few bytes against an already-hot string; the
    // assignment it avoids is a heap write.
    for (std::size_t i = 0; i < p.form.fields.size(); ++i) {
        const bool is_add = (i + 1 == p.form.fields.size());
        auto& f = p.form.fields[i];
        if (is_add) {
            if (f.label != "+") f.label = "+";
            const std::string_view want = p.numeric ? "type a port" : "type a path";
            if (f.help != want) f.help = want;
        } else {
            auto want = std::to_string(i + 1) + ".";
            if (f.label != want) f.label = std::move(want);
            if (!f.help.empty()) f.help.clear();
        }
    }
}

SandboxListPane build_sandbox_list(std::string_view row_id, std::string title,
                                   std::string help,
                                   const std::vector<std::string>& values,
                                   bool numeric) {
    SandboxListPane p;
    p.row_id = std::string{row_id};
    p.title = std::move(title);
    p.help = std::move(help);
    p.numeric = numeric;

    // One row per entry. The id carries the index so the readback order is the
    // display order -- a list where saving reorders your paths would be its own
    // small betrayal.
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto id = "e" + std::to_string(i);
        auto f = text(id, std::to_string(i + 1) + ".", {}, values[i]);
        // Caret at the END of the value, not at 0.
        //
        // A Text field defaults its cursor to 0, so moving onto an entry that
        // already reads "/opt/sdk" and typing put the character BEFORE the
        // path. Every text editor in the world puts the caret where the text
        // stops, and "append to what is here" is the only thing editing an
        // existing entry usually means.
        std::get<form::field::Text>(f.value).cursor = values[i].size();
        p.form.fields.push_back(std::move(f));
    }

    // A trailing blank row IS the add affordance.
    //
    // Not an "Add" button: a button would need its own key handling, its own
    // focus rules and a second way to create an entry. An empty row is already
    // editable by every rule the form has, and read_sandbox_list drops blanks
    // -- so "type into the empty row" adds, and "clear a row" removes, with no
    // new gesture to learn and no way to end up with a stray empty entry.
    const auto add_id = "e" + std::to_string(values.size());
    p.form.fields.push_back(
        numeric ? list_entry_number(add_id, "+", "type a port")
                : list_entry_text(add_id, "+", "type a path"));

    p.form.title = p.title;
    return p;
}

std::vector<std::string> read_sandbox_list(const SandboxListPane& p) {
    std::vector<std::string> out;
    for (const auto& f : p.form.fields) {
        const auto* t = std::get_if<form::field::Text>(&f.value);
        if (!t) continue;
        auto v = t->value;
        // Trim: a path with a stray space is a path that silently does not
        // match, which is exactly the failure the comma format had.
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (v.empty()) continue;   // blank == absent, for both kinds

        if (p.numeric) {
            // Ports are typed into a Text row so they can show a caret, so the
            // digits-and-range rule is enforced HERE instead of by the widget.
            // Anything that is not a port in range is dropped rather than
            // clamped: silently turning "8O80" into 8080 would be inventing a
            // rule the user did not ask for.
            if (v.find_first_not_of("0123456789") != std::string::npos) continue;
            unsigned long n = 0;
            try { n = std::stoul(v); } catch (...) { continue; }
            if (n == 0 || n > 65535) continue;
            out.push_back(std::to_string(n));   // normalised: drops "0443"
        } else {
            out.push_back(std::move(v));
        }
    }
    // Deduplicate, keeping FIRST position.
    //
    // A duplicate is never meaningful here -- two identical binds are one
    // bind, and two identical ports are one port -- but it is easy to create
    // by accident (type a path, scroll, type it again) and silently keeping
    // both makes the row's count disagree with what the policy does. Order is
    // preserved rather than sorted, because the user typed it in an order and
    // reshuffling their list on save is its own small betrayal.
    std::vector<std::string> uniq;
    uniq.reserve(out.size());
    for (auto& v : out)
        if (std::find(uniq.begin(), uniq.end(), v) == uniq.end())
            uniq.push_back(std::move(v));
    return uniq;
}

#if defined(__linux__) || defined(__APPLE__)

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
        // A capability the policy never asked for is not a gap in it. The three
        // resource caps ship at 0 = "no cap" on purpose (sandbox_config.hpp:
        // the right ceiling is a property of the machine), so without this the
        // default policy reports three `none`s that nothing is wrong with.
        w.not_requested =
            (id == CapId::mem_limit && cfg.memory_mb == 0) ||
            (id == CapId::cpu_limit && cfg.cpu_percent == 0) ||
            (id == CapId::pid_limit && cfg.max_procs == 0);
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

        // A cap the user did not ask for reads as a CHOICE, not a failure.
        // "none (none)" next to Memory (MB) = 0 is technically accurate and
        // tells the user nothing except that something might be broken.
        if (w->not_requested) {
            fld->origin = "no cap set";
            continue;
        }

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
                // Same reasoning as the footer: a cap nobody asked for is not
                // a wall that failed. Counting it made Balanced score 8/11 on
                // a host where every requested wall was strong, which reads as
                // "three things are broken" and is simply false.
                if (w.not_requested) continue;
                ++total;
                if (w.strength == "strong") ++strong;
            }
            if (total)
                fld->origin = std::to_string(strong) + "/" +
                              std::to_string(total) + " strong" + tense;
        }
    }

    // Empty path rows render as a blank line, which reads as broken rather
    // than as "nothing added yet". `origin` is the dim right-hand column, so a
    // word there says the row is working and empty without competing with the
    // wall annotations on the rows that have them.
    for (auto id : {kSbReadPaths, kSbWritePaths, kSbDenyPaths, kSbPorts}) {
        auto* fld = row_of(id);
        if (!fld || !fld->origin.empty()) continue;
        if (const auto* t = std::get_if<form::field::Text>(&fld->value))
            if (t->value.empty()) fld->origin = "none";
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

    // A path in BOTH a grant list and the mask list is a contradiction, and
    // it is one the compiler cannot catch: both produce valid mounts, and
    // which wins comes down to emission order. The user asked for two
    // incompatible things and the policy silently picked one.
    //
    // Checked here rather than resolved, deliberately. Picking a winner would
    // be inventing an intent -- "masked" and "writable" are equally plausible
    // readings of the same two rows -- so the pane says what it sees and lets
    // the user decide which row they meant.
    {
        auto clash = [&](const std::vector<std::string>& grants) {
            for (const auto& g : grants)
                for (const auto& d : cfg.deny_paths)
                    if (g == d) return g;
            return std::string{};
        };
        if (const auto bad = clash(cfg.write_paths); !bad.empty()) {
            if (auto* fld = row_of(kSbDenyPaths))
                fld->error = bad + " is also in Also writable \xc2\xb7 "
                             "remove it from one list";
        } else if (const auto bad2 = clash(cfg.read_paths); !bad2.empty()) {
            if (auto* fld = row_of(kSbDenyPaths))
                fld->error = bad2 + " is also in Also readable \xc2\xb7 "
                             "remove it from one list";
        }
    }

    // A relative path in any of the three lists.
    //
    // The sandbox resolves these against the CHILD's cwd, not the pane's, so
    // "build" means something the user cannot predict and usually matches
    // nothing at all. It compiles, it just quietly does not do what it reads
    // like -- the same class as the per-port rows above.
    for (const auto* entry : {&cfg.read_paths, &cfg.write_paths, &cfg.deny_paths}) {
        std::string_view id = entry == &cfg.read_paths    ? kSbReadPaths
                            : entry == &cfg.write_paths   ? kSbWritePaths
                                                          : kSbDenyPaths;
        auto* fld = row_of(id);
        if (!fld || !fld->error.empty()) continue;
        for (const auto& p : *entry) {
            if (p.empty() || p.front() == '/' || p.front() == '~') continue;
            fld->error = "`" + p + "` is relative \xc2\xb7 resolved against the "
                         "command's working directory, so it may match nothing";
            break;
        }
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
    // linked. It is a missing KERNEL: applying a plan needs namespaces,
    // landlock, seccomp and cgroup2 (Linux) or seatbelt (macOS). On Windows
    // claybin compiles a policy and refuses to apply it, and agentty has no
    // sandbox there for this pane to configure.
    Preview out;
    out.compiled = false;
    out.error = "no sandbox backend on this platform";
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
