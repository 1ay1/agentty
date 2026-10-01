# The sandbox boundary — design

How agentty decides what a tool may do, why that decision is **frozen for the
life of the process**, and what it takes to implement the remaining pieces
without lying to the user.

This is the design doc for the boundary itself. `docs/SANDBOX.md` is the
user-facing description of the backends; this one is about the model.

---

## 1. The one property everything here protects

> Whatever confined the first command in a session confines the last one too,
> and the user can find out what that is.

Two halves, and both are load-bearing.

**Fixed.** The policy is read once at startup and never changes. Not
"changes carefully", not "changes only when safe" — never.

**Legible.** At any moment there is exactly one answer to "what is enforcing
right now", it is displayed, and it is derived from the thing that actually
runs rather than restated by hand.

Every rule below follows from those two. When a rule and the property
disagree, the property wins and the rule is wrong.

---

## 2. Why the policy cannot be live

This is the part that is easy to get backwards, because every *other* setting
in agentty is live. Change the theme, the next frame is painted with it. That
is correct for a theme and wrong for a boundary, for three separate reasons.

**A live boundary only moves usefully in the weakening direction.**
Tightening mid-session is harmless and pointless: whatever already ran, ran
under the old walls. Loosening is the operation with value — and it is the
operation an attacker wants. agentty runs a language model that reads
untrusted text (web pages, issue bodies, dependency READMEs) and drives a
reducer. A prompt-injected agent that can reach the settings reducer, in a
world where the reducer can lower walls, has a supported path to lowering
them.

**The weakening is invisible and irreversible.** Switching claybin → bwrap
does not merely drop "some hardening". Measured, same workspace, same saved
policy:

| | `cat .env` in the workspace |
|---|---|
| claybin | zero bytes (masked) |
| bwrap | `SECRET=leaked` |

The credential mask list — `~/.ssh`, `~/.aws`, `~/.agentty/credentials.json`,
`.env`, `id_rsa`, `*.pem` — is applied only by the claybin path (§7). So a
backend switch silently un-masks secrets. And re-tightening afterwards does
not un-read a key that already left the machine.

**It makes the sandbox unauditable.** Two commands in one session under two
policies, with nothing in the transcript recording which got which. "What was
this command allowed to do?" stops having an answer. A weaker boundary you can
describe is worth more than a stronger one you cannot.

### The cost, stated honestly

You cannot try a tighter profile without relaunching. That is a real cost and
we pay it deliberately. The mitigation is that the pane compiles the policy
and shows the resulting walls *without spawning*, so the edit→understand loop
stays fast even though the edit→enforce loop needs a restart.

### Restart is the apply step

```
edit rows ──► ^S ──► settings.json ──► (restart) ──► set_config ──► sealed
                         │
                         └─► pane footer: "saved · applies on restart"
```

---

## 3. The model

Three values, three different lifetimes. Conflating any two of them has
already caused a bug in this codebase.

```
Mode          Off | Auto | On          what the user ASKED for   (CLI only)
Backend       None | Bwrap | Claybin   what the probe DELIVERED  (measured)
Config        the policy struct        what the walls SAY        (saved)
```

- **`Mode`** comes from `--sandbox` and nothing else. Not in the pane, not in
  `settings.json`. Turning the sandbox off is a launch decision, because a
  saved "off" is a foot-gun that survives reboots silently.
- **`Backend`** is never a claim, always a measurement. `available()` forks
  and attempts the uid_map write. The binary existing proves nothing —
  bubblewrap is routinely installed on hosts with unprivileged user
  namespaces disabled, and agentty shipped `sandbox: active` on exactly those
  (issue #21).
- **`Config`** is the user's saved policy. Sealed at startup.

### Sealing, concretely

```cpp
// src/tool/util/sandbox.cpp
std::shared_ptr<const sandbox_cfg::Config> g_cfg{};   // written once
std::atomic<bool> g_cfg_sealed{false};                // latches on first set
```

`set_config()` opens with an `exchange` on the seal. Second and later calls
log and return. **Enforced in code, not by comment** — it was a comment
("call before `init()`") and the settings pane broke it within a day of
existing. An invariant a future edit can violate by accident is not an
invariant.

`shared_ptr<const Config>` rather than a plain global because the value is
read from tool worker threads (`task_isolated`) and the UI thread at once.
`Config` holds three vectors and a string; assigning one under a concurrent
reader is a freed-buffer walk. That race was real, TSan flagged it, and the
seal makes it structurally impossible rather than carefully avoided.

`config_snapshot()` returns the shared pointer and is what the spawn path
uses: **one snapshot per command**, held for the whole posture build. Not
because the value can change now, but so that a future change to that rule
fails loudly at one call site instead of silently mixing two policies into
one sandbox.

---

## 3a. Hosts that deny user namespaces

The first real field report against this subsystem, and the reason claybin
earns its place as a second backend rather than a nicer one.

Ubuntu 24.04 ships an AppArmor profile that denies the `uid_map` write to
unconfined binaries. bwrap needs a user namespace to do *anything* — no userns,
no sandbox — so it dies with:

```
bwrap: setting up uid map: permission denied
```

The user's workaround was to write an AppArmor profile granting `userns` to
`/usr/bin/bwrap`. That works and is a reasonable thing to do, but it asks
someone to widen a system security policy to get a sandbox, which is the wrong
way round.

### What claybin does instead

**landlock and seccomp are unprivileged mechanisms.** Neither needs a
namespace, and AppArmor's userns restriction does not touch them. So claybin
degrades rather than failing. Measured through `compile()` on a host described
with `user_namespaces = false`:

| capability | with userns | without |
|---|---|---|
| `filesystem.read` | strong (mount-ns) | **strong (landlock)** |
| `filesystem.write` | strong (mount-ns) | **strong (landlock)** |
| `filesystem.exec` | strong (landlock) | **strong (landlock)** |
| `syscall.filter` | strong (seccomp-bpf) | **strong (seccomp-bpf)** |
| `resource.memory` | strong (cgroup2) | **strong (cgroup2)** |
| `privilege.drop` | strong | **strong** |
| `network.isolation` | strong (netns) | none |
| `process.isolation` | strong (userns+pidns) | none |

The plan drops from 29 ops to 9 and emits no `unshare` at all, so there is
nothing left to fail.

That is a **real sandbox where bwrap has none**: the filesystem boundary — the
one that keeps an approved command out of `~/.ssh` — survives intact, as does
the syscall filter. Losing network and pid isolation is a genuine downgrade,
and the wall report says so per capability, which is the honest way to ship a
partial boundary.

### Two changes this needed

1. **`claybin_backend::available()` no longer requires user namespaces.** It
   used to gate on `user_namespaces && mount_namespaces` — copying bwrap's
   precondition, and therefore copying bwrap's failure, on exactly the host
   that needed the alternative. The floor is now landlock **or** seccomp: with
   neither there is nothing to enforce, and claiming a sandbox would be the
   lie this subsystem exists to avoid.

2. **`probe()` falls back in both directions.** It tried claybin only when
   explicitly asked, then bwrap, then gave up. So the default (bwrap) failing
   meant no backend, even with claybin sitting right there. It now tries
   claybin as a last resort *after* bwrap has actually been tried — order
   matters: when both work the default stays bwrap, because a decade of
   upstream hardening beats a better feature list and nobody's boundary should
   change on upgrade.

### Toolchain paths

The same report hit a second problem: `go`/`gofmt` installed via
[webinstall.dev](https://webinstall.dev) live under `~/.local/opt` with links
in `~/.local/bin`, and the sandbox could not see them. Those are in
`kHomeToolSubdirs` now, along with `~/.local/xbin` (webinstall uses it for some
packages).

That list is the shared read set — **both** backends consume it, so a path
added there applies to both or neither. It deliberately excludes the broad
`$HOME` directories that mix tools with secrets: `~/.local/share`, `~/.npm`
(can hold an `_auth` token), `~/.config`, `~/.local/state`.

---

## 4. Precedence

```
CLI flag  ▷  saved config  ▷  shipped default
```

A flag typed this run beats a setting saved once. That forces an ordering in
`main.cpp` that looks wrong and is right:

```cpp
set_config(load_settings().sandbox);   // FIRST — also applies the saved backend
parse --sandbox-backend               // SECOND — overrides it
init(mode);                           // LAST — probes, caches the backend
```

`set_config` seals the *policy*, and the policy carries the backend, so it has
to run before the flag is parsed or an explicit `--sandbox-backend` would be
overwritten by the saved value. `init()` last because it probes, and a
preference set after the probe leaves `detected_backend()` disagreeing with
what was asked for.

`configured` is the discriminator. A config that never touched the pane
carries the shipped defaults and must not move the backend preference —
otherwise a bare `set_config()` silently undoes `--sandbox-backend`.

---

## 5. What the pane may and may not do

| | allowed |
|---|---|
| read the live policy | yes |
| read the pending saved policy | yes |
| write `settings.json` | yes |
| call `set_config` | **no** |
| change `Mode` | **no** |

The reducer stages `m.d.persisted.sandbox` and returns `persist_settings`.
That is the whole write path.

**The pane seeds from the pending saved config, not the live one.** These
differ the moment you save during a session. Seeding from the live policy
would silently discard an earlier save: set a profile, reopen, find your edit
gone.

**Locked rows still read back their current value.** Switching to bwrap must
not zero the claybin-only numbers — they are still the user's choices, merely
not in force. Losing them on a backend flip would make the pane destructive
to look at.

### The footer is not decoration

```
saved · applies on restart (the running sandbox is unchanged)
```

Everywhere else in agentty, saving means "in force now". This is the one
exception, so it has to be stated rather than assumed. A user who tightens the
syscall filter and keeps working would otherwise trust a wall that is not up.
That is the same class of lie as `sandbox: active` on a host that cannot
sandbox, pointed at the future instead of the present.

---

## 6. Per-backend capability, and the honesty rule

bwrap confines with **mount topology**. claybin adds seccomp, landlock and
cgroup2. So most rows mean nothing under bwrap.

| row | bwrap | claybin |
|---|---|---|
| readable scope, extra read/write | yes (binds) | yes (landlock) |
| masked paths | yes (mask mounts, §7) | yes (mask mounts) |
| secret scan depth | yes (feeds the mask list) | yes |
| network mode | no | yes |
| syscall profile | no | yes (§8) |
| W^X | no | yes |
| memory / procs / CPU% / tmp | no | yes (cgroup2) |
| open files / CPU secs / wall clock | no | yes (rlimit + cgroup2) |
| scope IPC | no | yes (landlock abi 6+) |
| hostname | no | yes |
| close inherited fds | yes | yes |

**The honesty rule.** A row the active backend cannot enforce is *locked,
with the reason*, and it is never silently accepted. The reason distinguishes
"switch Backend to claybin" from "claybin cannot start on this host", because
those want different actions from the user.

The corollary matters as much: a row bwrap *can* honour stays live. Locking
those would be a lie in the other direction.

The wall report is rewritten under bwrap for the same reason. Claiming
`filesystem.write: strong via landlock abi 10` for a wall bwrap will not build
is a *worse* lie than silence, because it arrives with a receipt.

### Lock by id, never by reference

```cpp
void lock_row(form::Form& f, std::string_view id, std::string_view reason);
```

Rows are built with `push_back`. A reference taken before a later push
dangles. The Resources block shipped that bug: four references bound, three
more pushes, then all four locked — so three writes landed in reallocated
memory and **the rows rendered unlocked**, which is the worst direction for
this to fail. An id lookup cannot dangle.

---

## 7. Fixed: bwrap applied no masks

*Resolved. Kept because the shape of the hole is worth remembering: it was
invisible for the one path anybody would test by hand.*

`kAlwaysMasked` / `kAlwaysMaskedNames` were referenced in exactly one place,
`build_claybin_posture`. `build_bwrap_argv(std::string_view shell_cmd)` did not
even take the config.

So under bwrap:

- `deny_paths` from the pane was dropped
- the non-configurable credential list was dropped
- `~/.ssh` happened to be safe anyway, because bwrap binds only
  `kHomeToolSubdirs` and unlisted `$HOME` paths are never mounted — safe by
  *whitelist*, not by masking
- a workspace `.env` **leaked in full**, because the workspace is bound
  read-write on both backends and scope cannot save it

That last pair is why it survived: the path a person checks first (`~/.ssh`)
was fine for an unrelated reason, and the path that actually leaked needed a
planted file to notice.

The header said that list is "not configurable, and that is the point: a
control the user can switch off to make their build work is a control that is
off." With a Backend row, it was switch-off-able. That sentence was the
specification and the code did not meet it.

### How it works now

`build_bwrap_argv` takes a config snapshot and emits, **after** the workspace
bind:

```
--ro-bind /dev/null <path>     # files, symlinks, sockets
--tmpfs             <path>     # directories
```

Same mechanism as claybin's `MountKind::mask`: make the path not *be* the
file. The guest sees an empty `~/.aws` rather than a denied one, which is the
better failure mode — a tool that reads it gets no credentials instead of an
EACCES it may report as a bug.

Three details that are not optional:

- **Stat to choose.** `--ro-bind /dev/null <dir>` fails and `--tmpfs <file>`
  fails. A path that does not exist needs no mask, and emitting one makes
  bwrap refuse to start on a host where the file is simply absent.
- **Order.** bwrap applies arguments in sequence and a later mount covers an
  earlier one, so a mask before the workspace bind is silently undone by it
  (§9). Asserted directly in `sandbox_escape_test`, since here the ordering
  *is* the argv order.
- **`.pem` is still uncovered.** It is a suffix rule, not a name, so it cannot
  be masked by path without walking the tree. Skipped identically in both
  backends so the two agree on what is covered — an asymmetry would be worse
  than the gap.

The Masked row is now live on both backends (§6).

### `.pem` and nested secrets: the bounded sweep

`kAlwaysMaskedNames` are **basenames** — `.env`, `id_rsa`, `*.pem`. A name
cannot become a mount without knowing where the file is, so the first version
masked only `<workspace>/.env` and missed the layout that actually occurs:
`services/api/.env` in any monorepo. `*.pem` could not be expressed at all,
because it is a *suffix* rule.

So `sandbox_cfg::mask_paths()` walks. One function, consumed by **both**
backends — that is the point. The credential list is the least negotiable part
of the policy, and a list applied by one backend and not the other is exactly
how `.env` leaked under bwrap while reading back empty under claybin. Computed
once, it is either right everywhere or wrong everywhere, and wrong everywhere
is much easier to notice.

It is **bounded**, and the bound is a setting (`mask_scan_depth`, default 3):

- it runs on **every spawn**, so an unbounded walk of a large tree would put a
  full directory scan in the latency path of every shell command
- `.git`, `node_modules`, `target`, `build`, `.venv` … are skipped. Two
  properties justify each: reliably enormous, *and* not somewhere a credential
  file legitimately lives. The second is the safety argument — if one of those
  ever becomes somewhere secrets do live, skipping it stops being a
  performance decision and becomes a hole.
- `.git` is the interesting case: no `.env`, but `.git/config` can carry a
  credential helper. Covered by the trust-handoff rules (§11) rather than by
  masking, so skipping it loses nothing.
- directory symlinks are **not** followed. Following one out of the workspace
  would mask host paths the user never asked about; one pointing back inside
  would walk the tree twice.
- a symlink *named* `.env` is masked as the link rather than followed —
  following it would mask the target and leave the link readable.

### Proof

`sandbox_live_check` plants a `.env` at the workspace root and reads it back
through the **real** bwrap argv. Verified by reverting the masking: it leaks →
red; restored → `cat: .env: Permission denied`.

It also plants `services/api/.env` and `services/api/key.pem`. Verified by
forcing the sweep depth to 0: both leak → red; at depth 3 both are masked.

One trap worth recording: `sandbox_escape_test` asserted that `~/.ssh` never
appears in the argv, which broke the moment masking landed — because a mask
*binds `/dev/null` to* that path. Presence in the argv conflated granting with
hiding; the check now distinguishes them by the preceding argument. A tightening
change should never fail a security test, and when it does the test is what is
wrong.

---

## 7a. Everything claybin can enforce is configurable

The pane used to expose a subset of claybin's policy surface, which meant the
engine could enforce walls the user had no way to ask for. Audited against
`claybin/policy/policy.hpp` and closed:

| claybin capability | pane row |
|---|---|
| `read` / `read_write` / `deny` | Readable scope, Also readable/writable, Masked |
| `memory` | Memory (MB) |
| `processes` | Processes |
| `cpu_percent` | CPU (%) |
| `open_files` | Open files |
| `cpu_time` | CPU seconds |
| `wall_clock` | Wall clock (s) |
| `tmpfs` size | /tmp size (MB) |
| `syscall_profile` | Filter |
| W^X | W^X |
| network isolation / ports | Access, Allowed ports |
| landlock IPC scoping | Scope IPC |
| `close_inherited_fds` | Close inherited fds |
| `hostname` | Report hostname as `sandbox` |

Three of those deserve a note, because they look redundant and are not:

- **`open_files` is not `processes`.** A runaway that leaks descriptors
  exhausts the host's file table without ever forking, so a pid cap does not
  bound it.
- **`cpu_time` is not `wall_clock` is not the tool timeout.** CPU time bounds
  total compute (a process that sleeps forever is untouched); wall clock bounds
  elapsed time (one that blocks forever is not); the tool layer's own timeout
  is a third thing that a child trapping SIGTERM can ignore — these cannot.
- **`hostname` is not a wall.** The guest cannot escalate either way. It is
  there because the real host name leaks into build output and test snapshots,
  which makes those non-reproducible. The row says so rather than sitting among
  the walls looking like containment.

**Deliberately not exposed:** `bind_fd` / `file_from_fd` (an fd is not
something a settings row can name), `keep_cap` (any capability inside a sandbox
is a hole — claybin already refuses `CAP_SYS_ADMIN`, and the rest have no
legitimate use here), `uid`/`gid` (changing them buys nothing the namespace
does not already give), `as_pid_1` (changes signal semantics in ways a user
cannot reason about), `isolation(microvm)` (no backend for it yet), and
`unsafe_inherit_fds` — which is named to be uncomfortable and should stay that
way.

---

## 8. Fixed: the default syscall profile broke ordinary tools

*Resolved in claybin. Kept here because the failure mode is the instructive
part, and because the reasoning is what a future widening has to match.*

`compiler_with_network()` denied eleven syscalls that normal CLI programs
need. Measured against the profile table, not guessed:

| syscall | who needs it | verdict |
|---|---|---|
| `mlock`, `munlock` | openssl pinning key material | allow |
| `nanosleep`, `clock_getres` | `sleep()`, retry backoff, timer setup | allow |
| `membarrier` | glibc / rseq fast paths | allow |
| `getcpu` | NUMA / topology probes | allow |
| `eventfd2`, `timerfd_create` | event loops | allow |
| `socketpair` | AF_UNIX pairs (git, ssh) | allow |
| `memfd_create` | anonymous temp files | allow |
| `mlockall` | gnupg, some JVMs | **stays denied** |

Observable effect: `curl` failed with `CURLE_OUT_OF_MEMORY (27)` on a machine
with gigabytes free, while `python3` fetched the same URL fine. `mremap` was
the same bug, fixed earlier — it was simply the one hit first.

**A filter whose failures lie about their cause is worse than a looser one.**
Error 27 sends you to memory caps and network rules. It took dumping the
profile table to find the real cause; a user gets "the sandbox broke curl",
which is indistinguishable from "the sandbox is broken".

### The bar that was applied

Authority, not convenience: *does this let the guest reach something it could
not already reach?* For the ten allowed, no — they pin the caller's own pages,
read a clock, or open a descriptor that refers to nothing outside the sandbox.
`socketpair` is AF_UNIX only and the net namespace still governs anything
routable.

`mlockall` is the deliberate exception, and the asymmetry with `mlock` is the
whole point: `mlock` pins a range the caller already owns, `mlockall` pins
*everything including future mappings*, which under a cgroup memory cap is a
denial-of-service lever rather than a convenience. EPERM, not a kill — it is a
refusal, not an attack signature.

### Why it stayed safe

claybin's own 20-test suite, including the 39-attack escape corpus and the
compile fuzzer, stays green. Widening a filter is exactly when that corpus
earns its keep.

aarch64 numbers were taken from the kernel's `asm-generic/unistd.h`, not
hand-translated from the x86_64 list — hand-translating is how `122`/`124` got
mislabelled the first time. The syscall table test resolves every name through
the kernel headers so a bad number fails a test instead of sitting in the
table.

### Proof

`sandbox_live_check` carries a curl case. Verified by reverting the widening
and watching it go `H:000` → red, then restoring it: `H:200` → green.

---

## 9. Ordering is the recurring bug

Three separate bugs in this subsystem were all ordering:

1. **mask before bind.** Masks applied, then the workspace root bound over
   them — so `<workspace>/.env` was masked and un-masked one line later. Every
   table-level test passed, because the mask really was in the policy; it just
   lost to a later mount. `$HOME` masks were unaffected, which is exactly why
   `~/.ssh` looked fine and hid it.
2. **`set_config` after the CLI parse.** Saved backend overwrote an explicit
   flag.
3. **preference after `init()`.** Probe cached, preference ignored.

The rule:

> **A mask must come after every bind that could cover it. A preference must
> come before the probe that caches it.**

State it in the code at each site. This class of bug is invisible to
policy-level tests by construction — the policy is right and the *application
order* is wrong — so it needs live verification (§10).

---

## 9a. The recurring bug: a control that is dead at one layer

Three times in this subsystem, a control existed at one layer and was inert at
another. Worth naming as a class, because the fix is the same each time and no
single-layer test catches any of them.

| what | looked like | actually |
|---|---|---|
| `kSbMode` | a row id, so a row | no row, no field, no reducer |
| `Observation` / `BlockedEvent` | a learning mode and a blocked feed, with a reducer arm | nothing ever populated them |
| the Masked row | a live setting on both backends | bwrap applied no masks at all |

The shape is always: **the layer that declares it and the layer that enforces
it disagree, and each is internally consistent.** A header compiles. A reducer
arm compiles. A row renders. The policy-level test passes. Only the child
notices, and only if you look.

### Two guards, and they catch different things

**`sandbox pane: every row round-trips into the config`** mutates every
editable row away from its value, reads the form back, and requires the config
to have changed. That catches a row misspelled at one of its two sites, or one
wired to nothing — verified by breaking `kSbOpenFiles` and `kSbMaskDepth` on
purpose, which report the offending row by name.

It **skips locked rows**, and that is a real limit worth stating: on a host
where claybin cannot start, every claybin-only row is locked, so the guard
covers only the handful that work on both backends. It would have caught
`kSbMode` and the dead `Observation` arm; it would NOT have caught a
claybin-only row wired to nothing, on a machine without user namespaces. The
live check is what covers those, and only where claybin runs.

**`sandbox_live_check`** proves the child experiences it. The round-trip test
would have passed happily while bwrap ignored every mask, because the config
was right; it was the *application* that was missing.

Neither is sufficient. A row has to survive both.

### The rule

> Do not declare a type, a row id, or a message for something that is not
> wired end to end. An unimplemented control reads as a shipped feature to the
> next person, and "we'll fill it in later" is indistinguishable from a
> security hole in review.

The learning mode and the blocked feed are genuinely worth building — "cargo
tried `ptrace(PTRACE_ATTACH)` and was denied" teaches what your toolchain does,
where "your build failed" teaches nothing and pushes people to turn the sandbox
off. But they are described in a comment now, not declared in a struct, until
something populates them. If the learning mode lands it must be **loud**: it is
a weaker sandbox while it runs, and it cannot be the default.

---

## 10. How this gets verified

Six layers. Each catches something the others cannot.

| layer | what it proves | runs |
|---|---|---|
| policy tests | the table says the right thing | always |
| `sandbox_pane_test` | door → open → edit → save → discard; the seal holds; **every row reaches the config** | always |
| `sandbox_broker_test` | the broker's decisions, driven directly (it is pure) | always |
| `sandbox_config_race_test` | concurrent readers see one whole policy | TSan lane |
| `sandbox_live_check` | **the child actually experiences it** | by hand |
| `sandbox_audit` | **which walls we claim at all, and how strongly** | by hand |

### The audit

```sh
cmake --build build --target sandbox_audit && ./build/sandbox_audit
```

It asserts nothing. It prints the real posture — the one
`build_claybin_posture()` produces, not a hand-written example — capability by
capability, with the mechanism behind each. "Is our sandbox any good" stops
being a matter of opinion: every `none` in the output is either a deliberate
trade or a gap, and the tool is what tells you which.

It prints two columns that matter: the **shipped default** (what a user who
never opens the pane gets — the number that matters most, because it is what
almost everyone runs) and the **ceiling** (everything the pane can ask for; a
`none` there is a real gap rather than a default).

Current state on a modern kernel, landlock abi 10, cgroup2 delegated:

| capability | default | ceiling |
|---|---|---|
| `filesystem.read` / `write` | strong (mount-ns) | strong |
| `filesystem.exec` | strong (landlock) | strong |
| `network.isolation` | partial (policy-only) | strong (netns) |
| `process.isolation` | strong (userns+pidns) | strong |
| `syscall.filter` | strong (seccomp-bpf) | strong |
| `resource.pids` | strong (cgroup2 pids.max) | strong |
| `resource.memory` | none | strong (cgroup2) |
| `resource.cpu` | none | strong (cgroup2) |
| `privilege.drop` | strong | strong |
| `device.isolation` | strong (dev allowlist) | strong |
| `host.kernel_isolation` | none | none |

The three `none`s are each a decision, not an oversight:

- **`resource.memory` / `resource.cpu` default off.** The right ceiling is a
  property of the machine and the build, and a wrong one turns a working
  `cargo build` into an OOM kill that looks like agentty's fault. There is no
  number that fits an 8-core laptop and a 64-core workstation, so a guess is
  worse than nothing. One row each, off until set.
- **`network.isolation` is partial by default** because the default is
  `NetMode::Full` — sharing the host netns so `git push` and `npm install`
  work. "policy-only" is the honest report for that: the policy records what
  is allowed, the kernel is not enforcing a boundary. `None` or `Ports` both
  reach `strong`.
- **`host.kernel_isolation` is `none` and will stay that way** on this
  backend. It means a microVM; see §13.

### The one default the audit changed

`resource.pids` used to be `none` by default, which meant an approved command
could fork without limit — a fork bomb was unbounded out of the box. Unlike
memory and CPU, **there is a number that is safe everywhere**: no legitimate
build needs 4096 concurrent processes (`make -j` on 64 cores peaks in the low
hundreds), and the pathological case wants millions. So `max_procs` ships at
4096.

Verified in the child, not in the report: the live check forks in a loop up to
20000 and gets `forked 4093`. A guarantee table saying "cgroup2 pids.max" and
a fork actually failing are different claims, and this subsystem has already
shipped the first without the second.

The live check is the one that matters for this subsystem, and it is
deliberately not a ctest: it needs working user+mount namespaces and makes a
real outbound connection. In CI it would skip itself and go green having
tested nothing.

```sh
cmake --build build --target sandbox_live_check && ./build/sandbox_live_check
```

Two rules learned the hard way, both from this harness giving a wrong answer:

**A negative test must prove the subject ran.** With the workspace directory
missing, every spawn failed on `mount(ENOENT)`, the child never executed, and
"did not reach the network" passed — because nothing reached anything. A spawn
failure is now a hard failure, and the harness creates its own workspace.

**A harness must mirror the real posture.** The first `.env` check left
`p.masked` empty, so it measured a posture nobody ever spawns and "failed" for
the wrong reason. If the harness builds a posture by hand, the hand-built one
has to match what `build_claybin_posture` produces.

**Verify a fix by breaking it.** Every fix above was confirmed by reverting it
and watching the check go red: revert the mask ordering → `SECRET=canary`
leaks; revert the mremap allow → `mremap EPERM`; revert the `subs_key` entry →
stale key router. A green test that was never red proves nothing.

---

## 11. Threat model — what this does and does not stop

**Stops:** an approved tool reading `~/.ssh` or a workspace `.env`; a command
reaching the network when the policy says no; a fork bomb or memory hog; the
trust-handoff class (agent writes a file a trusted host component later
executes — CVE-2026-48124 and friends) via `HandoffPolicy`; and a reducer
lowering the walls of the session it is running in.

**Does not stop:**

- **agentty's own API traffic.** In-process, never crosses the boundary.
- **Arbitrary in-process code.** Anything that can call agentty's own
  functions has already won — it could spawn unsandboxed. This is why
  `reset_config_for_test()` costs nothing against the threat the seal
  addresses: the seal defends against a *reducer bug*, not against an
  attacker with the process.
- **Network egress under `NetMode::Full`.** Read access plus network is read
  plus exfiltrate. That is why the default read set is narrow rather than
  convenient.
- **Secrets the name list does not know about, or that sit deeper than
  `mask_scan_depth` / inside a skipped directory.** The sweep is bounded by
  design (§7); the bound is a setting so the trade is the user's. A secret in
  `config.local.yaml` is invisible to a basename list regardless.
- **Windows.** No backend. `--sandbox on` fails loudly rather than pretending.

---

## 12. Working on this quickly

The subsystem is small but it sits inside `runtime_obj`, the tree's heaviest
object library, so the edit loop is worth getting right before you start.

**Use the `debug` preset.** `cmake --preset debug`. A bare `cmake -S . -B
build` defaults to **Release with LTO**, where a one-file change costs minutes
— I spent most of a session that way before noticing. The preset's display
name says "DEFAULT — fastest edit-build-test loop" and it means it.

**The loop is link-bound, not compile-bound.** Measured on GCC 16 / 12 cores /
mold, one-file change, reconfigure excluded:

| | compile | link | binary |
|---|---|---|---|
| `-g1` | 22 ms | 10.5 s | 801 MB |
| `-g1 -gsplit-dwarf` | 22 ms | ~4 s | 428 MB |

ccache already makes the compile free. Everything left was mold walking DWARF
the executable does not need at link time, so `AGENTTY_FAST_DEBUG` now adds
`-gsplit-dwarf`. gdb still resolves source lines — the `.dwo` files live beside
the objects and nobody ships a debug build.

**Why not the other knobs.** Both were already measured and left off for good
reasons that still hold: a shared PCH was net-negative (the libc++ prefix
expands to ~19 MB and the per-TU load cost exceeds the parse saving), and unity
builds win 3.2x *cold* but penalise incremental, which is the wrong trade for
this loop. Neither is the bottleneck anyway.

**The tree is instantiation-bound.** `subscribe.cpp` produces a 118 MB object,
of which 34,607 of 38,250 symbols are `std::variant` instantiations — 90%. That
is `Msg`: 264 leaves across 24 domains, and `std::visit` generates a 24×N
dispatch table per visit site. Adding `SandboxMsg` cost about 4% (113 MB → 118
MB), so it is structural rather than anything this subsystem did. Worth knowing
before you add a domain.

---

## 13. Kernel isolation: should claybin even have it?

The last `none` in the audit. The interesting question is not *can* we — it is
whether this belongs in claybin at all. I think it does not, and the reason is
about what claybin **is**.

### claybin's own premise forbids it

From its README: *"a drop-in replacement for bubblewrap, and the authority
compiler underneath it."* And from its CMakeLists:

```cmake
# the whole point: no libseccomp, no libcap, no libmount. raw syscalls only.
target_link_libraries(claybin PUBLIC)
```

Zero dependencies. 3 MB. Everything it does, it does by calling the kernel
directly — that is the product. A microVM backend would mean orchestrating two
external binaries (cloud-hypervisor, virtiofsd) plus a guest kernel image whose
provenance someone has to vouch for. That is not a stronger version of the same
thing; it is a different kind of software wearing the same name.

The lattice already says so, if you read it carefully. `Isolation` is
`process` → `hardened_process` → `microvm`, and the first two are *modes of the
same mechanism* — same syscalls, more of them. `microvm` is in that enum as the
honest top of the ordering, not as a roadmap item. It exists so a caller can
**ask** for a guarantee claybin will refuse, rather than asking for the strongest
thing claybin happens to implement and being quietly given less.

That is a real feature. `compile()` returning `"microvm backend not
implemented"` is more useful than a claybin that silently hands back a process
sandbox, and far more useful than a claybin with a half-finished VMM in it.

### What the measurement showed

This is not only an architecture argument; the numbers point the same way.
Measured on a developer box with working KVM — `firecracker 1.17.0` is one
`pacman -S` away, and `KVM_CREATE_VM` succeeds unprivileged because `/dev/kvm`
is mode 0666 there:

| what the process backend binds | size |
|---|---|
| `/usr` (the toolchain) | **31 GB** |
| the workspace, read-write | **16 GB** |

A rootfs image would have to contain or mirror that 31 GB, and the 16 GB
workspace would have to cross as virtio-fs with different file semantics. The
bind is not an implementation shortcut — it *is* the feature. An agent that
cannot see your actual `cargo`, your actual `node_modules`, and your actual repo
is not doing the job.

And **Firecracker cannot do it at all**: no virtio-fs support, not on their
roadmap (upstream issue #1180). It shares block devices, not directories — Lambda
does not need directories, because the function's code arrives as an image. So
the obvious answer ("use what Lambda uses") is also the wrong one. A microVM
backend here means cloud-hypervisor plus virtiofsd: three process lifetimes per
command instead of one, where a leaked virtiofsd outlives the command it served.

Cold start is ~125 ms against a `fork`+`unshare` well under a millisecond. For a
shell tool invoked dozens of times a turn, that is a different product.

### So where does it belong?

If agentty ever wants kernel isolation, it is a **third backend beside bwrap and
claybin** — not inside claybin — for a different job: running genuinely
untrusted code you do not intend to integrate, where a disk image is the right
model and 125 ms is fine. That is what Fly and Modal sell. It would be
`Backend::Microvm` in agentty's own enum, orchestrating external processes, and
claybin would stay what it is.

It is also not strictly stronger. A separate kernel does not give you landlock's
per-path rules or our credential masks, and §7's masking has **no analogue** over
virtio-fs — `--ro-bind /dev/null` is a mount trick, and the guest sees a
different tree. A microVM backend could easily end up *weaker* on the axis that
matters most here, which is keeping an approved command out of `~/.ssh`.

### What was built instead

Not a backend — an honest answer. The gap was already reported, but the *reason*
was not: the report said a flat `process-backend`, which invites "they could
have, and didn't". That is wrong on most CI runners and in every container
without `/dev/kvm` passed through.

So `probe_host()` probes KVM and the mechanism string distinguishes:

| report | means |
|---|---|
| `process-backend, kvm available` | we do not implement a microVM, but this host could run one — **the gap is ours** |
| `process-backend, no kvm` | the hardware boundary is unavailable here, so no backend could give it — **the gap is the host's** |

The probe **opens** `/dev/kvm` rather than stat-ing it, for the same reason
`probe_userns()` forks and attempts the uid_map write: the device exists on hosts
where it is root-only, which is the common desktop case. A capability that reads
as present and fails on use is the failure mode that file exists to avoid.
`O_RDWR`, because read-only access cannot create a VM.

`kvm` is otherwise **unused** by the process backend. It exists so one sentence
is measurable rather than a matter of opinion.

### The invariant to keep

`compile()` refuses `Isolation::microvm`. **Do not relax that into a downgrade.**
A caller asking for a separate kernel and silently getting a namespace is the
worst failure this subsystem could have — their threat model says "separate
kernel" and the reality says "shared". The refusal is what makes leaving the
capability unimplemented an honest position rather than a gap.

The compile fuzzer already asserts a process backend can never report
`host.kernel_isolation` as anything but `none`, so the invariant is
machine-checked rather than remembered.

---

## 14. Syscall brokering

This is what makes the sandbox a **capability system** rather than a filter. A
filter answers "may you call `ptrace`"; a supervisor answers "may you ptrace
**this pid**", at runtime, with the arguments in hand.

Before this, `ptrace` was a flat **kill** in the compiler profile — so `strace
prog`, `gdb`, and any test suite that traces a helper it spawned died outright.
That is safe and occasionally wrong, and "the sandbox killed my debugger" is how
people end up turning the sandbox off.

### The two hazards, which are structural

The kernel documentation is blunt about this: of
`SECCOMP_USER_NOTIF_FLAG_CONTINUE`, *"it should be absolutely clear that this
means the seccomp notifier cannot be used to implement a security policy."*
Both hazards are properties of the mechanism, not bugs to be careful about.

**1. TOCTOU on pointer arguments.** A notification carries the syscall's
*register* values. A pointer argument names memory in the guest, and the guest
has other threads — it can rewrite that memory between the moment we read it and
the moment the kernel acts. Reading a path out of the guest and then allowing
the call is a vulnerability. Container runtimes and CRIU have shipped this bug.

**2. CONTINUE is not a decision.** Telling the kernel to resume the syscall
re-runs it with the guest's credentials and the guest's view of memory, *after*
our check. Everything checked is stale.

claybin's broker refuses to expose either: a `Decision` is `allow`, `deny`, or
`inject_fd` (we did the work, the guest gets a descriptor). There is no
`continue`, and `Request` has no `path` field. So the policy here decides from
scalars only — and the moment a rule wants a path, that rule belongs in
landlock, which the kernel enforces with no race at all.

### What is brokered, and why only two things

| syscall | decided from | why broker rather than deny |
|---|---|---|
| `ptrace` | request + target pid (scalars) | a debugger attaching to its own child is normal; attaching to pid 1 is an escape |
| `kill` | target pid + signal (scalars) | reaping your own workers is routine; signalling outside the group is not |

A syscall qualifies only if **all three** hold: a scalar carries the decision, a
flat deny is sometimes wrong (so brokering buys compatibility), and a flat allow
is sometimes wrong (so there is a decision to make). Nothing involving a path
qualifies. `socket()` is scalar-decidable and claybin ships a helper, but the
network namespace already gives a stronger race-free answer, and two mechanisms
for one boundary is how they drift.

### The rules

- **`PTRACE_TRACEME` is allowed.** The child volunteers to be traced by its own
  parent; it grants no authority over anything else, and it is how `strace prog`
  works from the inside.
- **Target inside our own process group is allowed.** claybin calls `setsid()`,
  so the child leads its own group and descendants inherit it — that is what
  makes "inside my group" answerable from a register. Deliberately *not* "same
  pid namespace": we cannot ask the kernel that from a scalar, and guessing
  would be the almost-right check this whole file avoids.
- **An unknown `ptrace` request is denied regardless of target.** There are ~40
  requests and several (`PTRACE_SETREGS`, `PTRACE_POKEUSER`) rewrite a traced
  process's execution. "It is one of ours" is not a reason to permit an
  operation nobody reasoned about.
- **`kill(-1)` is denied.** It means "every process I may signal". Bounded
  inside a pid namespace — but §3a means we may not have one, so assuming is
  unsafe.
- **An unhandled brokered syscall is denied.** If the brokered list and the
  decision function ever drift, they must drift in the safe direction.

### Where the supervisor lives

In the **subprocess runner's existing poll loop**, beside the output pipe — not
in a thread of its own.

That is forced, not chosen. A brokered syscall blocks the guest *in the kernel*
until someone answers, so servicing the listener only between reads deadlocks:
the child waits for a decision, we wait for output it cannot produce until it
has one, and the idle watchdog fires on a child that was never idle. The runner
already owns the child's lifetime and already polls; a second loop would race it
for the same child.

Two subtleties that are easy to miss and cost a hang each:

- **The listener must still be polled after pipe EOF.** EOF does not mean the
  child is finished — it may close stdout and then make one last brokered call.
  Polling only the exit fd there means nobody answers, and the command dies on
  the hard deadline. The symptom would be "brokered commands sometimes take
  exactly the timeout", which is miserable to diagnose.
- **A finished listener must stop being polled.** `POLLHUP` is level-triggered,
  so a dead descriptor left in the set busy-waits at 100% CPU for the rest of
  the command.

A brokered syscall also **resets the idle window**: it is forward progress by
the child even though it produced no output, and not resetting would let the
watchdog kill a command that was legitimately waiting on us.

### The mandatory recheck

Immediately before responding, the supervisor calls `still_valid()`
(`SECCOMP_IOCTL_NOTIF_ID_VALID`). A notification id can be **reused** once its
thread is gone, so responding to a stale one would answer a *different* syscall
than the one judged. `seccomp_unotify(2)` documents this as mandatory rather
than advisory — the kernel offers no other way to close that race.

A stale notification is dropped and the loop continues: the listener itself is
fine, only that request is dead. Tearing down there would strand every later
brokered call.

### The audit trail is half the point

Every decision is logged at `Info` on the `Tool` channel:

```
ptrace ATTACH pid=1 DENIED
ptrace TRACEME pid=0 allowed
kill SIGTERM pid=4242 allowed
```

"Your build failed" teaches nothing. "cargo tried `ptrace ATTACH pid=1` and was
denied" teaches what your toolchain does, and is the difference between adding
one allowlist line and turning the sandbox off.

### The blocked-activity feed

The log is a trace; the **feed** is the security surface. `broker::record()`
keeps denials only — a feed listing every permitted syscall is an strace, and
the one denial that matters would drown in it — and the Sandbox pane renders the
three most recent above the wall report:

```
blocked: ptrace ATTACH pid=1 × 3  ·  kill SIGKILL pid=1  (+2 more in the log)
```

Denials sit *above* the walls because they outrank them: the wall report says
what **would** be enforced, the feed says what actually happened.

This is the feature §9a deleted for having no producer. The broker is what made
it real, because the broker is the only place that knows a denial happened *at
the moment it happens*, with the arguments still in hand.

Four properties, each load-bearing:

- **Bounded at 64 distinct events.** Unbounded growth is a denial of service the
  supervisor inflicts on its own host: a guest need only hammer distinct denied
  calls in a loop. The number is chosen for the *reader* — a pane showing more
  than a screenful has stopped communicating.
- **Oldest dropped, newest kept.** A guest trying to flush evidence of an early
  denial must push 64 *distinct* denials through, every one of which is itself
  recorded. The attempt is louder than the thing it would hide.
- **Identical consecutive events coalesce** into a saturating count. `× 4096`
  distinguishes a loop from a stray call; a count that wrapped to 0 would read
  as "this never happened", which is the worst lie an audit surface can tell.
  Coalescing is against the *last* event only, so two alternating denials stay
  interleaved — the order is information about what the toolchain was doing.
- **Written on worker threads, read on the reducer thread.** The one genuinely
  cross-thread surface in this subsystem. It uses `jaal::guarded<T>`, not a raw
  mutex: access is only possible through `with()`, so "forgot the lock" is
  unrepresentable rather than a review item. My first version used
  `std::mutex` and the concurrency banlist rejected it — correctly, and the
  replacement is better code, not a workaround.

  Verified by removing the lock: **135 TSan warnings** without it, **zero**
  with. The guard is load-bearing, not decoration.

### Verification

Two layers, because neither is sufficient:

- **`sandbox_broker_test`** drives `decide()` directly — it is pure, so "attach
  to pid 1" and "signal outside the group" are three lines each instead of
  staging a real escape. It caught a real permissive bug in the first version:
  an unknown ptrace request aimed at our own group was allowed, because the
  target check ran before the request check.
- **`sandbox_live_check` cases 9–11** run a real child against a real listener.
  Measured: `TRACEME` → allowed, `ATTACH pid=1` → `EPERM`, and 50 brokered calls
  in a row with no hang. Every one of those would *time out* rather than fail if
  the supervisor regressed, which is exactly why they exist.

Confirmed load-bearing by turning it off: with `broker = false` the child
produces no output at all — killed by seccomp, because `ptrace` is a `kill` in
the profile. With it on, `traceme 0 0`.

---

## 15. Implementation order

Each step is independently shippable and independently verifiable. Do not
batch them — the whole point of the live check is that it tells you which
change broke what.

1. **Land the seal.** `set_config` one-shot; reducer persists only; pane seeds
   from pending; footer states restart. Tests: seal holds against weaker
   *and* tighter re-installs, backend included. *(done)*
2. **Delete `kSbMode`.** A row id for a row that must not exist is an
   invitation to wire it up "for consistency". Mode is CLI-only (§3). *(done)*
3. **Fix the syscall profile** upstream in claybin (§8). Proof: the curl case
   in the live check goes from red to green, and the escape corpus stays
   green. *(done)*
4. **Mask under bwrap** (§7), respecting §9 ordering. Proof: the `.env` case
   passes with `backend=bwrap`. Then unlock the Masked row. *(done)*
5. **Re-audit the §6 table** against what the code now does, not what it did
   when the table was written. *(done — masked paths moved to "both")*

### Still open

- **Secrets deeper than `mask_scan_depth`.** A `.env` eight levels down is not
  masked at the default 3. Bounded on purpose (§7) — the bound is a setting, so
  it is a trade the user can make rather than one made for them.
- **Secrets inside a skipped directory.** A credential in `node_modules/` is
  not masked. Same trade, and the skip list is chosen so that each entry is
  somewhere a credential file does not legitimately live.
- **Names we do not know about.** The list is `kAlwaysMaskedNames` plus
  whatever the user adds. A secret in `config.local.yaml` is invisible to it.
- **The bwrap/claybin asymmetry in §6** is inherent, not a bug list. bwrap
  cannot express seccomp or cgroups; the honesty rule is the mitigation.

### Rules for whoever does this

- Never call `set_config` outside startup. If you need to, the design is
  wrong — change the design, not the call site.
- **Do not declare a control that is not wired end to end.** A row id, a
  struct, or a message for an unbuilt feature reads as shipped to the next
  person (§9a). Describe it in a comment; declare it when something populates
  it.
- A new pane row needs an entry in the §6 table and a lock reason. A row with
  no honest answer for one backend is not ready.
- A new form pane goes in **both** `subscribe()` and `subs_key()`. Present in
  one and not the other leaves the key router closed over a stale focus
  snapshot: dropdowns stop moving and input intermittently vanishes. Not a
  crash — a stale captured copy, which is the hazard `SubsKey`'s own header
  warns about.
- Prove enforcement in the child, not in the table (§10).
