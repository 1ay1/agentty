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
| `bastion` (Landlock) | Linux | `AGENTTY_SANDBOX_BACKEND=bastion` |

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

## bastion: path-set authority

[bastion](https://github.com/1ay1/bastion) answers the same question with
Landlock instead of mounts. Opt in:

```sh
AGENTTY_SANDBOX_BACKEND=bastion agentty
```

It is **opt-in rather than default** because it has been measured on one
kernel (7.2.2, Landlock ABI v10) and its own documentation marks the ABI
matrix below that unverified. Putting every user on a single measurement is
not a trade agentty should make for them. If bastion is unavailable or the
kernel is too old for a tier that actually contains anything, agentty falls
back to bwrap — never to running unsandboxed.

### Tiers

```sh
AGENTTY_SANDBOX_TIER=t3 AGENTTY_SANDBOX_BACKEND=bastion agentty
```

| tier | boundary |
|---|---|
| `t0`/`t1` | audit only — no enforcement is claimed |
| `t2` | path containment, shared network (**default**; matches bwrap) |
| `t3` | adds kernel-denied egress with a brokered per-host allowlist |

### Network allowlist (t3 only)

```sh
AGENTTY_SANDBOX_NET='github.com:443,registry.npmjs.org:443'
```

At **t3** this is real: the kernel denies direct outbound and bastion brokers
connections on loopback, so a compromised child cannot route around it. At
t2 the kernel matches sockets rather than hostnames, and bastion says so
instead of implying an allowlist it cannot enforce.

A refused connection is legible rather than a bare `ECONNREFUSED`:

```
bastion: egress REFUSED api.github.com:443
         remedy: bastion run -t t3 --net api.github.com:443 -- <cmd>
```

### Project policy

For anything beyond a couple of hosts, commit a policy instead of exporting
environment variables:

```sh
bastion observe -- ./your-build.sh     # run it once, record every access
bastion synthesize > .agentty/bastion.toml
```

agentty passes `.agentty/bastion.toml` to bastion automatically when the file
exists. Flags still combine with it, so the tier and ad-hoc grants remain
available without editing the policy.

This is deliberately the seam that scales. agentty **does not** ship a list
of allowed hosts, because what a spawned tool legitimately needs is a
property of *your* toolchain — your package registry, your git remote, your
internal mirror — which this layer cannot know and should not guess. A
synthesized policy is derived from evidence and reviewable in a diff.

## What is not covered

- **agentty's own API traffic.** In-process; the sandbox never sees it.
- **T0/T1.** No enforcement against a motivated adversary is claimed, and
  `bastion explain` prints the real boundary rather than a reassuring one.
- **Windows.** No backend. `--sandbox on` fails loudly rather than pretending.

## Verifying

```sh
bastion doctor                      # backend, max tier, ergonomic floor
bastion explain --tier t3           # the boundary actually enforced
```

`bastion doctor` also asserts an *ergonomic floor* — writable `$TMPDIR`,
toolchain caches, `/dev/null`, DNS iff egress is granted — and refuses to
hand over a profile while one is violated. That is a security property, not
a convenience: a sandbox that breaks the toolchain makes agents thrash, and
thrashing agents get their sandboxes switched off.
