# Sandboxing

agentty wraps the commands it runs — `shell`, `diagnostics`, `git`,
`process_start`, lifecycle hooks and external ACP agents — in an OS sandbox.
It does **not** wrap its own provider traffic: that is in-process and never
crosses this boundary. What the sandbox constrains is what a *tool* can do.

```
--sandbox auto   wrap when a backend is available (default)
--sandbox on     wrap, and refuse to start if no backend is available
--sandbox off    never wrap
```

The sandbox policy is **fixed when agentty starts** and does not change while
it runs. The Sandbox settings pane edits the policy for your *next* launch and
says so; there is no way to loosen the boundary of a session already in
progress, deliberately. `--sandbox off` is a launch flag for the same reason:
a saved "off" is a setting that silently survives reboots.

That is a real cost — you cannot try a tighter profile without relaunching —
and it buys one property: whatever confined the first command in a session
confined the last one too. The pane compiles your edits and shows the walls
they would produce without running anything, so you can see the effect before
you restart.

For the reasoning, the per-backend capability table and the known gaps, see
[docs/design/sandbox-boundary.md](design/sandbox-boundary.md).

## Backends

| backend | platform | selected |
|---|---|---|
| `claybin` | Linux | **default** |
| `bwrap` (bubblewrap) | Linux | fallback |
| `sandbox-exec` | macOS | default |

On Linux, `claybin` is the default because it is strictly stronger and the
difference is measured, not asserted — agentty's bwrap path emits no
`--seccomp`, no cgroup limits and no landlock:

| capability | claybin | bwrap |
|---|---|---|
| filesystem read/write | strong (mount ns) | strong (mount ns) |
| filesystem exec | strong (landlock) | partial (binds only) |
| syscall filter | strong (seccomp-bpf) | **none** |
| memory / cpu / pids caps | strong (cgroup2) | **none** |
| ptrace + kill brokering | yes (seccomp-notify) | **none** |

There is no capability where bwrap wins. Run `sandbox_audit` to print this
report for your own host.

`bwrap` stays as the fallback because the two fail on *different* hosts, not
because it is a safer default. bwrap dies where unprivileged user namespaces
are denied (Ubuntu 24.04's AppArmor profile); claybin needs landlock, which a
kernel older than 5.13 does not have. Asking for claybin on a host that cannot
run it gives you bwrap, never "no sandbox" — opting into the stronger backend
must not cost you your boundary.

Override with `--sandbox-backend bwrap|claybin`, or the Backend row in the
Sandbox settings pane.

The startup banner names the one you actually got:

```
agentty: sandbox: active (claybin)
```

Availability is a **capability probe**, not a `which`. agentty runs a real
confinement attempt before claiming a sandbox, because the binary existing
proves nothing — bubblewrap is commonly installed on hosts where unprivileged
user namespaces are disabled, and agentty used to report `sandbox: active`
on exactly those (issue #21).

## bwrap: what it does and does not do

Bubblewrap builds confinement out of **mount topology**: the workspace is
bound read-write, system directories read-only, `/tmp` is a fresh tmpfs.
Two consequences are worth stating plainly.

**The network is shared.** `--share-net` is passed so `git push`, `npm
install` and `curl` keep working. A command you approve can reach any host.
This is an accepted residual, not an oversight — but it is a residual.

**The bind list is hand-maintained.** Roughly twenty `--ro-bind` arguments
plus specific `/etc` files and `$HOME` toolchain caches (`.cargo`, `.npm`,
`.dotnet`, `.sdkman`, …). A toolchain nobody anticipated needs a patch here.

## What is not covered

- **agentty's own API traffic.** In-process; the sandbox never sees it.
- **The network.** The net namespace is shared, so an approved command can
  reach any host. This is a deliberate trade — sandboxing egress would break
  `git push`, `npm install` and `curl`, which are flows users expect to work —
  but it means read access plus network is read plus exfiltrate. That is why
  the read set above is narrow rather than convenient.
- **Windows.** No backend. `--sandbox on` fails loudly rather than pretending.

## Verifying

The banner states the backend, and it is a capability probe rather than a
`which`, so "active" means a real confinement attempt succeeded:

```sh
agentty --sandbox on            # refuses to start if no backend works
```

To check the boundary rather than trust it, run something that should be
denied and confirm it is:

```sh
agentty --sandbox on run 'run: cat ~/.ssh/id_rsa'
```

Paths outside the grant are not bound into the sandbox at all, so they report
as **absent** rather than denied — `No such file or directory` is the expected
answer there, not evidence that the file is missing.
