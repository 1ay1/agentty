# Sandboxing

Every shell command agentty runs — the `bash` tool, `diagnostics`, git, lifecycle
hooks, ACP terminals — runs inside an OS-enforced sandbox. This is the reference
for what that boundary is, how to configure it, and what it deliberately does
not cover.

Two companion documents:

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
| `claybin` | Linux | **default** |
| `bwrap` (bubblewrap) | Linux | fallback |
| `claybin` | macOS | **default** |
| `sandbox-exec` | macOS | fallback |
| — | Windows | none (documented gap) |

### Why claybin is the default

Because the difference is measured, not argued. agentty's bwrap path emits no
`--seccomp`, no cgroup limits and no landlock:

| capability | claybin | agentty's bwrap path |
|---|---|---|
| filesystem read/write | strong (mount ns) | strong (mount ns) |
| filesystem exec | strong (landlock) | partial (binds only) |
| network isolation | strong (netns) | none (`--share-net`) |
| syscall filter | **strong** (seccomp-bpf) | **none** |
| memory / cpu / pids caps | **strong** (cgroup2) | **none** |
| privilege drop | strong | strong |
| ptrace + kill brokering | yes (seccomp-notify) | **none** |

There is no capability where bwrap wins. bubblewrap *can* take a seccomp program
on an fd; agentty does not give it one, because writing a BPF compiler to feed it
would be rebuilding claybin inside the bwrap path.

### Why bwrap is still here

The two fail on **different hosts**, so this is a fallback, not legacy:

- **bwrap dies** where unprivileged user namespaces are denied — Ubuntu 24.04's
  AppArmor profile blocks the `uid_map` write. That host is why claybin exists.
- **claybin dies** where its walls do not exist. Landlock landed in 5.13, so a
  RHEL/CentOS kernel older than that has none of it.

Deleting bwrap would not leave those users with claybin — it would leave them
with `Backend::None`. Trading a weaker boundary for **no** boundary, on the
machines least equipped to notice, is issue #21 wearing a different hat.

Asking for claybin on a host that cannot run it yields bwrap, never nothing.

Override with `--sandbox-backend bwrap|claybin`, or the Backend row in the pane.

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

### Availability is a probe, not a `which`

On Linux, `available()` forks and attempts the real `uid_map` write. On macOS
it forks and actually calls `sandbox_init()`. A binary on `$PATH` that cannot
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
| engine refused | `claybin cannot start on this host · restart falls back to bwrap…` |
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
- **A new form pane goes in *both* `subscribe()` and `subs_key()`.** Present in
  one and not the other leaves the key router closed over a stale focus snapshot:
  input silently vanishes. This has been hit twice.
- **Anything written in the present tense derives from `HostFacts`, never from
  `Config`.** The config is what the user *wants*; only `HostFacts` knows what
  they *have*.
- **Prove enforcement in the child, not in the table.**
