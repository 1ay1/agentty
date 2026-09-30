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
| masked paths | **no** (§7) | yes (mask mounts) |
| network mode | no | yes |
| syscall profile | no | yes (§8) |
| W^X | no | yes |
| memory / procs / CPU / tmp | no | yes (cgroup2) |
| scope IPC | no | yes (landlock abi 6+) |
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

## 7. Gap: bwrap applies no masks

`kAlwaysMasked` / `kAlwaysMaskedNames` are referenced in exactly one place,
`build_claybin_posture`. `build_bwrap_argv(std::string_view shell_cmd)` takes
no config at all.

So under bwrap:

- `deny_paths` from the pane is dropped
- the non-configurable credential list is dropped
- `~/.ssh` happens to be safe anyway, because bwrap binds only
  `kHomeToolSubdirs` and unlisted `$HOME` paths are never mounted — safe by
  *whitelist*, not by masking
- a workspace `.env` **leaks in full**, because the workspace is bound
  read-write on both backends and scope cannot save it

The header says that list is "not configurable, and that is the point: a
control the user can switch off to make their build work is a control that is
off." With a Backend row, it currently is switch-off-able. That sentence is
the specification; the code does not meet it.

### Implementation

Teach the bwrap path to mask. One empty-file bind per path, after the
workspace bind:

```
--ro-bind /dev/null <path>           # files
--tmpfs             <path>           # directories
```

`build_bwrap_argv` must take the config, like `build_claybin_posture` does.
Then unlock the Masked row for bwrap and move it to the "both" side of the
§6 table.

Ordering is the trap. See §9.

---

## 8. Gap: the default syscall profile breaks ordinary tools

`compiler_with_network()` denies eleven syscalls that normal CLI programs
need. Measured against the profile table, not guessed:

| syscall | who needs it |
|---|---|
| `mlock`, `munlock`, `mlockall` | openssl pinning key material |
| `membarrier` | glibc / rseq fast paths |
| `memfd_create` | anonymous temp files |
| `eventfd2`, `timerfd_create` | event loops |
| `socketpair` | AF_UNIX pairs (git, ssh) |
| `nanosleep` | `sleep()`, retry backoff |
| `clock_getres` | timer setup |
| `getcpu` | NUMA / topology probes |

Observable effect: `curl` fails with `CURLE_OUT_OF_MEMORY (27)` on a machine
with free RAM, while `python3` fetches the same URL fine. `mremap` was the
same bug, already fixed — it was simply the one hit first.

**A filter whose failures lie about their cause is worse than a looser one.**
Error 27 sends you to memory caps and network rules. It took a profile-table
dump to find the real cause; a user gets "the sandbox broke curl", which is
indistinguishable from "the sandbox is broken".

### Implementation

Decide what the profile is *for* before widening it. `compiler_with_network`
currently aims at "a compiler can build". The rows offer it as the default for
running arbitrary tools. Those are different profiles and the name should say
which.

Then, for each of the eleven, the question is authority, not convenience:

- `mlock`/`munlock` — bounded by `RLIMIT_MEMLOCK`; grants no reach. Allow.
- `nanosleep`, `clock_getres`, `getcpu`, `membarrier` — no authority. Allow.
- `eventfd2`, `timerfd_create`, `socketpair` — new descriptors, no new reach.
  Allow; `socketpair` is AF_UNIX only and the net namespace still governs
  sockets.
- `memfd_create` — anonymous memory that can be made executable. Allow, but
  note it interacts with W^X and should be re-checked under `wx_protect`.
- `mlockall` — can pin all memory; a DoS lever under a cgroup cap. Keep
  denied unless something real needs it.

This is claybin's security surface, so it lands upstream in claybin with the
live check as the proof, not as a local patch.

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

## 10. How this gets verified

Four layers. Each catches something the others cannot.

| layer | what it proves | runs |
|---|---|---|
| policy tests | the table says the right thing | always |
| `sandbox_pane_test` | door → open → edit → save → discard; the seal holds | always |
| `sandbox_config_race_test` | concurrent readers see one whole policy | TSan lane |
| `sandbox_live_check` | **the child actually experiences it** | by hand |

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
- **bwrap masking, today.** §7.
- **Windows.** No backend. `--sandbox on` fails loudly rather than pretending.

---

## 12. Implementation order

Each step is independently shippable and independently verifiable. Do not
batch them — the whole point of the live check is that it tells you which
change broke what.

1. **Land the seal.** `set_config` one-shot; reducer persists only; pane seeds
   from pending; footer states restart. Tests: seal holds against weaker
   *and* tighter re-installs, backend included. *(done)*
2. **Delete `kSbMode`.** A row id for a row that must not exist is an
   invitation to wire it up "for consistency". Mode is CLI-only (§3).
3. **Fix the syscall profile** upstream in claybin (§8). Proof: the curl case
   in the live check goes from red to green, and the escape corpus stays
   green.
4. **Mask under bwrap** (§7), respecting §9 ordering. Proof: the `.env` case
   passes with `backend=bwrap`. Then unlock the Masked row.
5. **Re-audit the §6 table** against what the code now does, not what it did
   when the table was written.

### Rules for whoever does this

- Never call `set_config` outside startup. If you need to, the design is
  wrong — change the design, not the call site.
- A new pane row needs an entry in the §6 table and a lock reason. A row with
  no honest answer for one backend is not ready.
- A new form pane goes in **both** `subscribe()` and `subs_key()`. Present in
  one and not the other leaves the key router closed over a stale focus
  snapshot: dropdowns stop moving and input intermittently vanishes. Not a
  crash — a stale captured copy, which is the hazard `SubsKey`'s own header
  warns about.
- Prove enforcement in the child, not in the table (§10).
