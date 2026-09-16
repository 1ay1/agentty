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

## Backends

| backend | platform | selected |
|---|---|---|
| `bwrap` (bubblewrap) | Linux | default |
| `sandbox-exec` | macOS | default |

The startup banner names the one you actually got:

```
agentty: sandbox: active (bwrap)
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
