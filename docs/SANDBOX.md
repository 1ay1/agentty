# Sandboxing

Every shell command agentty runs — the `bash` tool, `diagnostics`, git, lifecycle
hooks, ACP terminals — runs inside an OS-enforced sandbox. This is the reference
for what that boundary is, how to configure it, and what it deliberately does
not cover.

Two companion documents, plus the user guide
([Sandboxing](website/sandboxing.md): pick a posture, what commands can reach):

- [`docs/design/sandbox-boundary.md`](design/sandbox-boundary.md) — the design
  record: why each decision was made, which bugs forced it, and what was tried
  and rejected. Read it before changing anything here.
- `tests/sandbox_audit.cpp` — a runnable report of what your host actually
  enforces. Build and run it; it prints the table in [Capabilities](#capabilities)
  for your machine rather than for ours.

---

## The one property

**An approved command must not be able to read a credential it was never
granted, and must not be able to write outside the workspace.**

Everything below serves that. The failure mode this subsystem is built around is
not "the user set a switch wrong" — it is **a boundary that reports itself as
active while enforcing nothing**. agentty shipped exactly that once (issue #21:
`sandbox: active` while every command died on a `uid_map` denial), which is why
so much of the design is about *measuring* rather than *claiming*.

---

## Quick start

| you want | do this |
|---|---|
| see what is enforcing right now | the startup banner, or the Sandbox settings pane |
| tighten everything at once | Sandbox pane → **Posture** → `hardened` |
| stop anything phoning home | Posture → `airgapped` |
| a build that breaks under the filter | Posture → `permissive` (keeps the mount walls) |
| turn it off entirely | `--sandbox off` |
| require it, fail if unavailable | `--sandbox on` |

Settings persist as you edit and apply **on the next launch** — see
[Why the policy is sealed](#why-the-policy-is-sealed).

---

## Backends

| backend | platform | selected |
|---|---|---|
| `claybin` | Linux | only engine |
| `claybin` | macOS | **default** |
| `sandbox-exec` | macOS | fallback |
| `claybin` | Windows | **default** |
| — | Windows fallback | **none, by design** |

On Windows there is no fallback engine and no bwrap: the probe either proves
it can build a wall (AppContainer or a job object) and claybin runs, or
`sandbox: active` becomes `sandbox: unavailable, running unsandboxed` and
commands run unconfined —
reported at startup, never silently. Windows Sandbox (the Hyper-V VM product)
was deliberately not chosen: it is a whole-OS VM with seconds of boot time, no
pipe-stdout model and no per-command exit codes, which breaks the
spawn/adopt/stream/kill contract every tool call relies on. AppContainer is
the process-level translation of the same policy; the kernel-isolation gap it
carries is reported per capability rather than hidden (see
[claybin on Windows](#claybin-on-windows)).

### Why claybin is the only Linux engine

bwrap was removed. agentty's bwrap path emitted no seccomp, no cgroup limits
and no landlock, so it only ever gave the mount walls. The case for keeping it
as a fallback was hosts where claybin can't run, but claybin needs only
landlock **or** seccomp, and seccomp (Linux 3.5) predates the unprivileged
user namespaces bwrap needs (3.8). Every host that could run bwrap can run
claybin.

If claybin can't start on a host, the pane says so and commands run
unconfined; `--sandbox on` refuses to start instead.

`--sandbox-backend bwrap` still parses and runs claybin, so old scripts keep
working. The pane has no Backend row.

### claybin on macOS

Same engine, same policy, a different set of walls underneath. claybin's
front end is one description of the boundary compiled per platform, so the
read set, the masks and every extra grant in the pane mean the same thing on
a mac as on Linux — they are the *same code path*, not a parallel one.

What differs is what the OS will enforce:

| capability | claybin on Linux | claybin on macOS | `sandbox-exec` |
|---|---|---|---|
| filesystem read/write | strong (landlock + mount ns) | **strong**† (seatbelt) | strong, fixed profile |
| filesystem exec | strong (landlock) | **strong**† (seatbelt) | strong, fixed profile |
| network isolation | strong (netns) | **strong** denying, partial allow-listing | none |
| syscall filter | strong (seccomp) | **none** | none |
| memory / cpu caps | strong (cgroup2) | partial (rlimits) | none |
| pid caps | strong (cgroup2) | **advisory** (RLIMIT_NPROC is per-UID) | none |
| ptrace + kill brokering | yes (seccomp-notify) | **none** | none |
| per-path grants from the pane | yes | **yes** | no |
| guarantee report | yes | **yes** | no |

† `strong` for a same-path policy. agentty's own posture adds a `proc_fs`, a
`dev_fs`, a `tmpfs` and a mask — none of which have an access-control reading
that is *exactly* the tree reading — so its plan's fidelity is `approximate`
and the filesystem rows report `partial`. See
[Capabilities](#capabilities) for the measured output.

The two macOS engines use the *same kernel mechanism* — claybin calls
`sandbox_init(3)` with a compiled profile, `sandbox-exec` is Apple's CLI
wrapper around it — so this is not a choice about strength. It is a choice
about what survives the trip. The `sandbox-exec` path concatenates one fixed
profile string, so the pane's settings have nowhere to go; the claybin path
compiles the policy you actually configured, and reports per capability which
walls the kernel agreed to build.

Two honest gaps, stated rather than papered over:

- **No syscall filter.** macOS has no seccomp equivalent available to an
  unprivileged process, so `Isolation::hardened_process` is *refused* rather
  than quietly downgraded, and brokering does not exist here.
- **rlimits are not cgroups.** An rlimit is per-process and inherited; a
  cgroup counts a tree. Four children under a 1 GB cap can use 4 GB between
  them. `RLIMIT_NPROC` is weaker still — it counts processes for the whole
  UID, so another terminal window moves the limit, which is why it reports
  `advisory` rather than `partial`.

Asking for claybin on a mac where seatbelt is unavailable yields
`sandbox-exec`, never nothing — the same fallback discipline as Linux, in both
directions.

### claybin on Windows

Same engine, same policy, the third set of walls. `compile()` is pure and
unit-tested on every host; the apply step (`claybin/windows/spawn.cpp`) uses
one AppContainer profile per capability set, stamps DACL/integrity ACEs for
the granted paths (opt-in — without it the report honestly says `partial`),
wraps resource limits in a job object, and maps the policy's syscall posture
onto process mitigation policies.

| capability | claybin on Windows | note |
|---|---|---|
| filesystem read/write/exec | partial | AppContainer default-deny + per-object ACEs |
| network isolation | partial (Full/Ports) · **strong** (None) | no net capability = no socket |
| process isolation | **strong** (AppContainer token) | the wall Linux builds with user+pid ns |
| syscall filter | partial | mitigation policies, no programmable filter; `hardened_process` is refused |
| memory / cpu / pids caps | **strong** (job object) | as hard as cgroup v2 |
| privilege drop | host-dependent | `strong` only where the probe proves the restricted token + AppContainer combination survives; measured per host, never assumed |
| device isolation | partial | AppContainer object namespace |
| host kernel isolation | **none** | process backend, by definition |

#### The apply step, in order

`sandbox.cpp` builds the same `Posture` it builds on every other platform and
hands it to `claybin_backend::spawn_shell`, which on this platform is
`spawn_argv({"cmd.exe", "/d", "/s", "/c", <command>})`. From there:

1. **`build_policy(posture)`** — the identical sealed policy the Linux and
   macOS arms compile. The read set, the masks, the network mode and the
   resource caps are the *same values*, not a Windows restatement.
2. **`::clay::windows::compile(sealed, probe_host(), opts)`** — one
   AppContainer profile per capability set (claybin suffixes the profile name
   with a hash of the capability set, so two different policies never share
   one), plus the restricted token, the job limits and the mitigation list.
3. **`opts.allow_host_acl_mutation = true`**, set once, deliberately. ACL
   stamping mutates the host, so claybin keeps it opt-in at the API level;
   agentty opts in here because the alternative is an AppContainer that can
   reach nothing the user asked the command to reach — a sandbox that is
   useless rather than safe. The guarantee report still grades the filesystem
   rows `partial`, because per-object ACLs are not a path-prefix model.
4. **Environment** — `GetEnvironmentStringsW` is read and converted to UTF-8,
   with any per-call overrides layered on top.
5. **`spawn()`** returns `Spawned{pid, process, job}`. The two `int` handle
   fields of `SpawnResult` carry **HANDLEs, not descriptors**: `pidfd` is the
   child's PROCESS handle (the waitable exit, which jaal's
   `windows_process::adopt` consumes) and `kill_tree` closes in on the JOB
   handle. Supervision — poll, drain, idle deadline, grace, reap — stays in
   `subprocess.cpp`/`exec.cpp`, unchanged and shared with the unsandboxed
   path.
6. **No broker.** `supervisor_fd` and `service_broker` keep their sentinel
   defaults: seccomp user-notify has no Windows equivalent, so `ptrace`/`kill`
   brokering (§ [Syscall brokering](#syscall-brokering)) does not exist here.

#### Read roots and the header floor

The Windows read set is the POSIX one translated, and it is resolved from the
environment rather than hardcoded — Windows is not always on `C:`:

| what | from | why |
|---|---|---|
| `%SystemRoot%` | env, fallback `C:\Windows` | loader, DLLs, `System32` |
| `%ProgramFiles%`, `%ProgramFiles(x86)%` | env | installed tools |
| `%ProgramData%` | env | the `/etc` half: package metadata, host ssh config |
| `%USERPROFILE%` tool dirs | env | `.cargo\bin`, `\go\bin`, `\.nvm`, `\.pyenv`, `\.rbenv`, `\.bun\bin`, `\.deno\bin`, `\.dotnet` |
| `%SystemDrive%\` | env, for `HostReadable` only | the convenient scope, named for what it is |

A build note that bit the first Windows release: `claybin` sets
`_WIN32_WINNT=0x0A00` **PUBLIC** on the target, because MinGW defaults to
pre-Win8 headers and those hide the AppContainer APIs entirely. Without it
`spawn.cpp` does not compile at all — the failure is a missing declaration,
not a runtime refusal.

#### Two behaviors specific to this platform, measured not assumed

- **The privilege-drop probe is process-lifetime cached.** On some Windows
  builds (insider 26300 measured) the restricted-token + AppContainer
  combination is process-warmth dependent: early probes fail, later ones
  pass, and a re-probe per spawn could build a wall that kills every guest
  (0xC0000142 during ntdll init). The probe therefore runs the child in the
  exact production spawn shape — SUSPENDED, job-assigned, handle-listed —
  three times, and the first (coldest) verdict sticks for the process.
- **Cancel kills the tree first.** An adopted child carries no job handle in
  jaal, so a graceful stop can only terminate the leader; a grandchild
  (`ping` inside `cmd`) survives and holds the stdout pipe open, which turned
  a 2-second Esc into a 20-second drain. The exec loop fires
  `TerminateJobObject` (via the sandbox's `kill_tree`) before the graceful
  stop; the grace remains as fallback. The closure behind `kill_tree` *owns*
  the job handle and `CloseHandle`s it when it drops, at which point
  `KILL_ON_JOB_CLOSE` takes down anything the command left behind — the same
  ownership that fixed a job-handle leak per sandboxed command, after which
  whatever a command left running had survived a normal exit.

`/proc` and `/dev` policy roots map to a virtual object-namespace sentinel:
there is nothing to stamp an ACL on, so grants on them are skipped and the
AppContainer's default-deny carries the isolation. Run `sandbox_audit` on a
Windows host for the live wall list; `tests/sandbox_win_posture_test.cpp`
pins the exec chain end to end.

#### The settings pane

`preview_sandbox` compiles the pane's policy with claybin's Windows compiler,
the same one the spawn path uses and with the same ACL opt-in, so the footer
and the per-row walls show what commands get: filesystem `partial` via
appcontainer+DACL, memory and pids `strong` via the job object, syscall filter
`partial` (mitigation policies, no programmable filter).

Two env-gated traces exist for the stop/spawn paths, because those are the
ones where a bug shows only under load and a rebuild-only diagnosis costs a
codebase-scale rebuild:

| variable | what it prints | where |
|---|---|---|
| `AGENTTY_STOP_TRACE=1` | every reactor wait (deadline requested vs actual), every event with its token, the stop/tree-kill/force sequence, and the drain + feeder-join tail of `run_child` | `src/tool/util/exec.cpp` |
| `CLAYBIN_TRACE=1` | one line per spawn: which API was used, whether the appcontainer and restricted token were applied, capability count, limits, workdir, flags, and whether `CreateProcess` itself succeeded | claybin `src/windows/spawn.cpp` |

Both cost one `GetEnvironmentVariable` per call when unset and are silent
then; the cancel bug above was root-caused with them and they stay for the
next one.

### Availability is a probe, not a `which`

On Linux, `available()` forks and attempts the real `uid_map` write. On macOS
it forks and actually calls `sandbox_init()`. On Windows it creates and
deletes an AppContainer profile and runs a real child under the compiled
walls (the verdict is cached for the process lifetime — see
[claybin on Windows](#claybin-on-windows)). A binary on `$PATH` that cannot
start a namespace — or a symbol that is present but refused — is not an
available backend, and treating it as one is precisely how `sandbox: active`
came to mean nothing.

The macOS probe runs in a **child** for a reason beyond honesty: a seatbelt
profile is irreversible. A process that enters one cannot leave it, so an
in-process probe would confine agentty itself.

---

## Postures

Twenty-eight individually-correct switches is still the wrong thing to hand
someone who wants "tighter than this". The **Posture** row is a preset that
writes every other row:

| posture | what it is | what it costs |
|---|---|---|
| `permissive` | mount walls only | no syscall filter, no memory/cpu/time caps |
| `balanced` | the default, byte for byte | network is open, so read access is also exfiltration |
| `hardened` | every wall claybin can build | a build needing an unusual syscall may fail |
| `airgapped` | hardened + no network at all | `git push`, `npm install`, `curl` all stop working |
| `custom` | not selectable | what the pane *reports* when the rows match no preset |

**`permissive` keeps the fork-bomb cap.** It drops memory, CPU, file-descriptor
and time limits, because the right ceiling for those is a property of the
machine and a wrong guess turns a working `cargo build` into an OOM kill that
looks like agentty's fault. `max_procs` is the exception the field comment
already argued for: there *is* a number safe everywhere (no build needs 4096
concurrent processes; `:(){ :|:& };:` wants millions). It used to be zeroed
along with the rest, and `0` means *unlimited* at the boundary — claybin sets
no rlimit and no `pids.max` at all — so permissive shipped an unbounded fork
bomb. That is the exact finding `tests/sandbox_audit.cpp` was written for,
reintroduced through a preset. Permissive means "the walls are off", not "the
machine is forfeit".

### What the label asserts — and what it doesn't

`detect_posture` is a fixpoint test, so a field `apply_posture` does not *write*
is outside the posture's definition by construction. Two such fields are
genuinely enforced: `write_paths` and `allow_ports`. A config can therefore read
**Hardened** while carrying extra grants you added.

That is deliberate — invariant 3 below says a preset never touches your path
lists, because wiping a project's `read_paths` when you *tried* a preset would
make presets hostile to explore. So read the label as **"the walls this preset
sets are in force"**, not "this is all there is". The pane shows all 28 rows for
exactly this reason (invariant 1): the label summarises, the rows are the truth.
Pinned by `sandbox_pane_test` so the reading is on the record rather than
inferred.

`airgapped` is the one worth singling out: it is the only configuration where
the admission in [Threat model](#threat-model) — *read access plus network is
read plus exfiltrate* — stops being true.

Three properties make presets safe rather than a second source of truth:

1. **A preset writes the rows, it does not replace them.** All 28 stay visible
   and editable. A preset that hid the detail would be another "sandbox:
   active" — a label standing in for a boundary you can no longer inspect.
2. **The label is derived, never stored.** `detect_posture()` asks
   `apply_posture()` what each preset would produce and compares. A stored field
   would drift the moment you edited one row, and the pane would claim
   "Hardened" for a config that is not. Deriving it means the label can only be
   right, or `Custom`.
3. **A preset never touches the engine or your path lists.** Picking `hardened`
   on a host where claybin cannot start must not silently switch backends, and
   wiping your project's `read_paths` because you tried a preset would make
   presets hostile to explore.

---

## Capabilities

What a policy compiles to, per capability. Run `sandbox_audit` for your host.
On Linux with claybin, every wall exists:

```
=== everything the pane can ask for
  filesystem.read        strong  mount-ns
  filesystem.write       strong  mount-ns
  filesystem.exec        strong  landlock
  network.isolation      strong  netns
  process.isolation      strong  userns+pidns
  syscall.filter         strong  seccomp-bpf
  resource.memory        strong  cgroup2 memory.max
  resource.cpu           strong  cgroup2 cpu.max
  resource.pids          strong  cgroup2 pids.max
  privilege.drop         strong  no_new_privs+empty bounding set
  device.isolation       strong  dev allowlist
  host.kernel_isolation  NONE    process-backend, kvm available
```

The same policy on macOS, where several do not — and the report is the only
reason you would know which. This is real `sandbox_audit` output on darwin 24,
with every pane row turned up:

```
host: darwin=24 seatbelt=1
      sbpl: paths=1 net=1 proc=1
      rlimits=1
      hypervisor=1  (a microvm backend COULD run here)

=== everything the pane can ask for
  filesystem.read        partial  seatbelt
  filesystem.write       partial  seatbelt
  filesystem.exec        partial  seatbelt
  network.isolation      strong   seatbelt (deny network*)
  process.isolation      partial  seatbelt (inherited profile, shared pid space)
  syscall.filter         partial  seatbelt operations (no syscall filter on macOS)
  resource.memory        partial  RLIMIT_AS (per-process)
  resource.cpu           partial  RLIMIT_CPU (per-process)
  resource.pids          advisory RLIMIT_NPROC (per-UID, not per-tree)
  privilege.drop         partial  seatbelt profile survives exec (no user namespace)
  device.isolation       partial  seatbelt /dev path filters
  host.kernel_isolation  NONE     shared host kernel (use a vm for isolation)
```

Three rows in that table are worth reading closely, because each is a place
the report refuses to round up:

- **`network.isolation: strong`** — the one macOS row that matches Linux.
  `(deny network*)` is a single rule the kernel enforces exactly, equivalent
  to an empty netns in what the guest can reach. Note it is `strong` only
  here, with the network turned *off*; an endpoint allow-list reads `partial`,
  because seatbelt matches on the address and the name-to-address step happens
  in userspace where DNS can answer differently next time.
- **`resource.pids: advisory`**, not `partial`. That is not hedging:
  `RLIMIT_NPROC` counts processes for the whole UID, so another terminal
  window moves the limit. It is a number the guest tends to respect, not a
  wall, and the vocabulary has a word for that.
- **`filesystem.*: partial`** — not because seatbelt is weak, but because
  agentty's posture includes a `proc_fs`, a `dev_fs`, a `tmpfs` and a mask.
  Those have no access-control interpretation that is *exactly* the tree
  interpretation (a tmpfs becomes "you may write here", losing "starts empty"
  and "is size-capped"), so the plan's fidelity is `approximate` and the
  ceiling drops with it. A same-path-only policy reports `strong`.

`host.kernel_isolation` is `none` on **every** process backend, by definition —
the sandboxed command shares your kernel. Only a microVM changes that, and
[§13 of the design doc](design/sandbox-boundary.md) explains at length why
claybin should not grow one. The report says `kvm available` or `no kvm` so the
gap is attributed honestly rather than hidden.

### The honesty rule

The pane never claims a wall it is not building:

- the subtitle describes **what is running**, derived from a host probe, never
  from the row you just moved
- a row that this backend or kernel cannot enforce is **locked with the reason**,
  not hidden — hiding it makes the limitation invisible
- each row carries the mechanism enforcing it (`strong via landlock abi 10`),
  because a strength without its receipt is an unfalsifiable claim
- walls for an engine that is not yet running are marked `(on restart)`

---

## Secret masking

Credential files are masked — bound over with an empty file — whether or not you
configure anything. This is not optional, and that is the point: a control you
can switch off to make your build work is a control that is off.

Three sources, in order of how much they are your business:

1. **`kAlwaysMasked`** — `$HOME` credential paths. `.ssh`, `.aws`, `.gnupg`,
   `.kube`, `.docker/config.json`, `.config/gh`, `.config/gcloud`, `.azure`,
   `.netrc`, `.npmrc`, `.pypirc`, `.cargo/credentials`, `.git-credentials`, and
   agentty's own credential store. Not configurable.
2. **`kAlwaysMaskedNames`** — credential *basenames*, found by walking the
   workspace to `mask_scan_depth`. Not configurable except for the depth.
3. **`deny_paths`** — whatever you add in the pane's **Masked** row. Applied
   last, so an explicit deny of yours cannot be undone by one of ours.

### Why the walk exists

A basename rule cannot become a mount without knowing where the file is. Masking
only `<workspace>/.env` missed `services/api/.env`, which is where it actually
lives in any monorepo.

The walk is **bounded** (`mask_scan_depth`, default 3) because it runs on every
spawn — an unbounded scan of a large tree would put a full directory walk in the
latency path of every shell command. It skips `.git`, `node_modules`, `target`,
`build`, `.venv`: reliably enormous *and* not where credentials legitimately
live. Directory symlinks are not followed.

### Name rules vs suffix rules

Entries are exact basenames (`.env`, `.npmrc`, `id_ed25519`, `credentials.json`)
except those marked with a leading `*`, which match by suffix (`*.pem`,
`*.tfvars`).

The marker is **explicit rather than inferred from shape**. Inferring "starts
with a dot, no second dot" looks tidy and silently promotes `.env` and `.npmrc`
to suffixes — so `prod.env` and `scoped.npmrc` start being masked as a side
effect of adding an unrelated entry. Widening a security rule by accident is as
bad as narrowing one, and nobody reviewing the list would see it.

---

## Syscall brokering

The sandbox does not only *filter* syscalls — for two of them it **decides at
runtime, with the arguments in hand**. `ptrace` and `kill` are brokered through
seccomp user-notification: a self-`PTRACE_TRACEME` is allowed, `PTRACE_ATTACH` to
pid 1 is denied, and the process keeps running either way instead of being killed.

That turns the sandbox from a filter into a capability system — "may you ptrace
**this** pid" rather than "may you ptrace".

### Why the broker is small, and must stay small

seccomp user-notification is the sharpest footgun in Linux sandboxing, and the
kernel documentation says so outright. Two hazards, both structural:

1. **TOCTOU on pointer arguments.** A notification carries the syscall's
   *register* values. A pointer names memory in the guest, and the guest has
   other threads — it can rewrite that memory between our read and the kernel's
   act. Reading a path out of the guest and then allowing the call is a
   vulnerability, not a feature.
2. **`CONTINUE` is not a decision.** It re-runs the syscall with stale checks
   after the supervisor answers.

claybin refuses both: the decision is allow/deny only, and the request carries no
path field. **Policy decides from scalars alone.** Any rule wanting a path belongs
in landlock, where the kernel resolves it.

A syscall qualifies for brokering only if all three hold: a scalar argument
carries the decision, a flat deny is sometimes wrong, and a flat allow is
sometimes wrong. That is why the list is two entries long.

---

## The trust-handoff gate

The sandbox confines the **process**. It cannot see the escape that matters most
in practice:

> the agent writes a perfectly ordinary file, inside the workspace it is allowed
> to write, which something **outside** the sandbox executes later.

A `.vscode/tasks.json`, a git hook, a venv interpreter, a `.envrc`. By the time
VS Code runs that file there is no sandbox in the picture at all. Pillar
Security's July 2026 series reproduced this across Cursor, Codex, Gemini CLI and
Antigravity — in almost none of them did the agent break the sandbox.

So the gate sits on the **tool call**, before the bytes land, on all four
authoring tools: `write`, `edit`, `apply_patch`, `move`. (For `move` the
*destination* is what matters — the source being inert is exactly the attack.)

| policy | behaviour |
|---|---|
| `refuse` (default) | the call fails with a message naming the mechanism and the alternative |
| `warn` | the write happens and is recorded in the feed |
| `allow` | no gate |

`refuse` is the default because this is not a "might be risky" prompt. The cost
of a false negative is an escape; the cost of a false positive is one declined
tool call that tells the model exactly what to do instead.

The feed records **allowed** handoffs too. A feed that only listed refusals would
go blank exactly when you have turned the gate off — which is when you most need
to see what the agent is authoring.

`--sandbox off` disables this too. "Off" has to mean off.

---

## Telling the model

A model that does not know it is sandboxed **misdiagnoses every wall it hits**.
`EPERM` becomes "your corporate proxy is blocking this"; a blocked connect becomes
"check your firewall". The agent then either sends you chasing a phantom or
retries a call the kernel will never allow.

Two mechanisms, and the second matters more:

1. **The system prompt** gets a stanza in `<environment>`, next to the OS and
   shell — it is the same class of fact. It states what is *allowed* (not just
   what is blocked), because a denial list invites probing the edges while
   "write here" tells the model where to work on the first try.
2. **The error gets annotated.** The prompt is thousands of tokens away by the
   time a `Permission denied` arrives; the model is looking at the output. A
   failed command whose output looks like a wall gets a `[sandbox]` note naming
   the cause and forbidding the two wrong moves — do not retry unchanged, do not
   blame the firewall.

Only on failure, and only when the output matches. A note on every command is
noise the model learns to skip, which is how an explanation stops working.

---

## Why the policy is sealed

The pane writes to disk for the **next** launch. Restart is the apply step.
Everything else in agentty applies live; this is the one exception.

A boundary that can move mid-session only ever moves usefully in the loosening
direction — and loosening cannot be undone, because re-tightening does not
un-read a key that already left. It would also make the sandbox unauditable: two
commands in one session would get two different walls, with nothing recording
which got which.

The pane says so, and — since the promise is checkable — it *checks* rather than
asserts. `restart_outcome()` reports when the next launch will **not** deliver
what the rows describe:

| state | footer |
|---|---|
| clean | `applies on restart, verified against this host` |
| will not compile | `will NOT apply on restart · <reason>` |
| engine refused | `claybin cannot start on this host · commands will run unconfined` |
| degraded | `applies on restart, but this host cannot enforce: …` |

---

## Threat model

**Stops:** an approved command reading `~/.ssh/id_rsa`, `~/.aws/credentials` or a
workspace `.env`; writing outside the workspace; forking a bomb; exhausting host
memory; ptracing another process; mapping W+X pages; reaching the network when
you said not to; the agent authoring a file your host later executes.

**Does not stop:**

- **Exfiltration over an allowed network.** With network on, a command that can
  read can also send. This is a deliberate trade — sandboxing egress would break
  `git push`, `npm install` and `curl` — and it is exactly why the default read
  set is narrow rather than convenient, and why `airgapped` exists.
- **Secrets deeper than `mask_scan_depth`,** or inside a skipped directory. The
  bound is a setting, so the trade is yours.
- **Credential names we do not know about.** A secret in `config.local.yaml` is
  invisible to a basename list.
- **Kernel-level escape.** The command shares your kernel. A kernel LPE defeats
  every process backend, which is what `host.kernel_isolation: none` means.
- **agentty's own API traffic.** In-process, never crosses the boundary.
- **Windows.** No backend. `--sandbox on` fails loudly rather than pretending.

---

## Verifying

Three layers, deliberately:

| | what it proves | run it |
|---|---|---|
| `sandbox_audit` | what your host enforces, per capability | `cmake --build build --target sandbox_audit && ./build/sandbox_audit` |
| `sandbox_live_check` | the child actually experiences it — spawns real commands and reads the results | `cmake --build build --target sandbox_live_check && ./build/sandbox_live_check` |
| the ctest suite | every table-level and reducer-level property | `ctest --test-dir build -R sandbox` |

The middle one exists because the gap between "the plan says X" and "the child
experiences X" is where real bugs live. A missing `mremap` in the syscall profile
once made `curl` report "out of memory" on a machine with free RAM — every
table-level test passed while `realloc()` failed in the guest.

The startup banner states the backend, and it is a capability probe rather than a
claim:

```
agentty: sandbox: active (claybin)
```

---

## For contributors

Read [`docs/design/sandbox-boundary.md`](design/sandbox-boundary.md) first. The
rules that matter most:

- **Never call `set_config` outside startup.** If you need to, the design is
  wrong — change the design, not the call site.
- **Do not declare a control that is not wired end to end.** A row id, a struct
  or a message for an unbuilt feature reads as shipped to the next person. This
  has caused three separate bugs here: `kSbMode`, the handoff row that enforced
  nothing for months, and the two feed types that existed before their producers.
- **A new pane row needs an entry in the per-backend table and a lock reason.** A
  row with no honest answer for one backend is not ready.
- **Anything written in the present tense derives from `HostFacts`, never from
  `Config`.** The config is what the user *wants*; only `HostFacts` knows what
  they *have*.
- **Prove enforcement in the child, not in the table.**
