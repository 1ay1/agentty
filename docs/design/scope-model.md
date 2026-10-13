# Scope — the config-resolution algebra — design

## The problem this fixes

Five config concerns each answered "where does this live, who placed it
there, and may I execute it" with a *different, incompatible* shape:

- **memory** — a `Scope{User,Project}` enum → one path; Project gated on
  writability.
- **skills / agents / commands** — a six-root ladder (`.agentty`,
  `.agents`, `.claude` under the project, then the same three under `~`),
  hand-written **three times**, first-name-wins shadow.
- **MCP** — `resolve_config()`: env ▷ project ▷ user, **one winning file**,
  and a coarse env-var trust gate (`AGENTTY_MCP_ALLOW_PROJECT`).

Those are five partial functions pretending to be total —
`config_path(bool project)`, `resolve_config()` returning an empty path on
a miss, enums that forget where a value came from. Every one re-hardcoded
precedence, and none could tell you *which file* a resolved value came from
(so an editor couldn't write back to the right place — the root of the MCP
"toggled the wrong file" bug).

`agentty::scope` replaces them with one pure algebra they all fold through.

## The three axes (kept separate on purpose)

The word "scope" smeared together three orthogonal things. Naming them as
distinct types is what makes the model clean:

```
Locus     WHOSE config          Explicit ▷ Local ▷ Project ▷ User
Dialect   the DIRECTORY tribe   .agentty (native) ▷ .agents ▷ .claude
Trust     may I EXECUTE it      Trusted | Pending | Blocked
```

- **`Locus`** is a *precedence lattice* — declaration order **is**
  resolution order, so precedence is defined once and every feature
  inherits it. `Explicit` (an env-pointed file) is most authoritative;
  `User` (global) least. `Local` — project-private and *uncommitted* — is
  reserved between them (see "Local", below).
- **`Dialect`** is a separate axis so the six-root ladder is the *product*
  `Locus × Dialect`, generated once, instead of four hand-written arrays.
  Within a locus the native `.agentty` dir shadows the interop conventions.
  **Breadth is per-feature**, declared on the `Layout` (`dialects`), not a
  global list — see "Dialect breadth" below.
- **`Trust`** is bound to **content**, never inferred from `Locus` (see
  "Trust", below).

## The model

```
Source                     (a resolved origin — carries its own provenance)
  locus       (Explicit | Local | Project | User)
  dialect     (Agentty | Agents | Claude)
  base        (the concrete <…>/.agentty dir this maps to)
  writable    (is this a valid WRITE target here?)

Tagged<T>  { value: T, source: Source }   (every resolved item knows its origin)
Layout     { leaf, explicit_env, dialects } (what the FEATURE stores — passed in)
Env        { home, project_root, … }      (the resolved edge — passed by value)
```

Two properties do the heavy lifting:

- **Provenance is a value, not a recomputation.** A `Source` is resolved
  once and travels with each item. Config-*without*-a-source is
  unrepresentable, so an edit targets `Source::base` — never a re-derived
  `config_path(bool)`. That structurally kills the "wrote to the wrong
  file" bug class.
- **Scope knows nothing about its callers.** There is deliberately **no**
  enum of features here — no filenames, no env vars baked in. A caller
  hands scope a `Layout{leaf}` (`"memory.jsonl"`, `"skills"`, …); scope
  lays out roots and folds. Inverting that dependency is what keeps this a
  reusable primitive rather than a registry of everything downstream.

## Dialect breadth — a property of the FEATURE

Which dialects a feature reads answers a question only that feature can:
**do the same bytes work in both tools?**

| kind | example | dialects | why |
|------|---------|----------|-----|
| portable content | skills, agents, commands | `kPortable` (all three) | a `SKILL.md` is valid in another tool, so reading theirs is free compat |
| our own format | memory, `mcp.json` | `kNativeOnly` (default) | nobody else writes that file there; extra roots are dead paths |

This used to be one array inside `plan()`, shared by everyone, so the ladder
always carried three dialects per locus and MCP filtered the extras back out
downstream (`if (src.dialect != Agentty) continue;`). **Two decision sites for
one decision**: `plan()` reported six sources where three were read, so
anything built on it — a `config where` command, a doc, a test — described
behaviour that did not exist. Adding a dialect *looked* like it widened MCP
and silently did nothing, because the downstream filter still dropped it.

Moving the choice onto `Layout` collapses that to one site, and flips the
default to the safe end: a feature gets `.agentty` only and must opt **into**
interop. Forgetting now fails closed — one fewer directory read, instead of a
cloned repo's `.claude/` read by accident.

A dialect earns a slot only when the same bytes work in both tools. That test
is what rules `~/.ai/mcp/mcp.json` out: it swaps the *leaf shape*
(`mcp/mcp.json`) and has no project-side counterpart, so forcing it in would
turn `plan()` from a product back into a hand-written path list. Foreign
config belongs behind an explicit import, not an implicit read.

## The fold — two monoids over one source list

The elegant core. `plan(Layout, Env)` emits the ordered `Source` list
(Locus-major, Dialect-minor). Two resolvers fold it; they differ only in
how they *combine* what each source yields:

| resolver | monoid | used by |
|----------|--------|---------|
| `resolve_first` | **override** — first present source wins the whole value | memory, hooks, MCP-as-one-file |
| `resolve_union` | **union** — merge all, first-key-wins shadow, provenance kept | skills, agents, commands |

`resolve_union`'s first-key-wins rule *is* "project ▷ user, native ▷
interop" for free — the shadow every discovery feature wanted, implemented
once.

## Purity contract (this is a TEA codebase)

Resolution is a pure function of an explicitly-passed `Env`. Nothing in the
fold dips into `getenv` or the cwd: the process edge builds an `Env` once
(a feature owns *its own* edge resolution — memory's `Env`, for instance,
carries a richer `getpwuid_r` home fallback), then every resolver is a
deterministic fold a test drives with a fabricated `Env`. Errors flow
through `scope::Result = std::expected<T, scope::Error>` — the house
algebraic-error idiom, not a magic empty path.

## Trust (the MCPoison fix, stated as a type)

Cursor's CVE-2025-54136 ("MCPoison") pinned trust to a server's **name**,
so an attacker could swap the command under an approved name and no
re-prompt fired — silent, persistent RCE. `scope::trust_of(source,
content_sha, approvals)` states the fix as a type:

- `Explicit` / `User` config is **implicitly trusted** — the human placed
  it, and both are outside a cloned repo's reach.
- `Project` / `Local` executable config starts **`Pending`** and becomes
  `Trusted` only when *that exact content hash* is approved. Change the
  bytes → the approval is void → re-gate. Approvals persist **outside** any
  committed file, so a cloned repo can never approve its own servers.

This generalises hooks' proven content-hash approval into one primitive
MCP will adopt when it migrates off the coarse env-var gate.

**Now the ONE trust primitive across the codebase.** Hooks originally had
their own bespoke content-hash store (predating scope); they've been
consolidated onto `scope::Approvals` too (reading their legacy `{path:
hash}` format transparently on upgrade). So every executable-from-untrusted-
origin surface — plugins and hooks — now shares one content-hash trust
mechanism, and lower-risk injection surfaces (project-defined agents) are
surfaced via provenance rather than gated. Trust is applied in proportion
to risk, through a single implementation.

## `Local` — shipped as a value, not yet wired

`Locus::Local` (project-private, gitignored — the "evaluate a server before
you commit it to the team" tier) exists in the lattice so the algebra is
complete and future-proof. But `plan()` emits **no** `Local` sources yet:
it's wired into a resolver only when a real second consumer appears, to
avoid a lattice value only one feature ever reads.

## Migration status

Adopted smallest-first, each change **behaviour-preserving**:

| feature | resolver | status |
|---------|----------|--------|
| memory | override (locus × native) | ✅ migrated |
| skills / commands / agents | union ladder | ✅ migrated |
| **MCP** | union + provenance + trust | ✅ fully migrated |

MCP folded through scope in stages. **Stage A+B:** `read_config_servers()`
*unions* project + user mcp.json via `scope::plan` (first-writer-wins
shadow) instead of picking one winning file, each server tagged with its
`Source`; the plugin picker shows both scopes, badges provenance, and
routes every edit (remove / toggle server / toggle tool) to the server's
*own* `Source::base` — fixing the long-standing "toggled a project server,
silently edited the user file" bug, and the false `no "command"` error on
HTTP/SSE servers. **Stage C:** the `AGENTTY_MCP_ALLOW_PROJECT`-only
connect-gate is replaced with content-bound trust — a workspace-local
config's stdio servers connect only when the human has vouched for it,
either via the env opt-in (back-compat) or an approval of the file's
content hash (`scope::content_hash` + a user-root `Approvals` store, via
`load_approvals`/`save_approvals`). Editing the file changes the hash and
re-gates it — the MCPoison fix, live. `plugin::is_project_config_trusted()`
/ `approve_project_config()` are the grant/query API; an untrusted project
server shows "untrusted project config — approve to enable" in the picker.

Trust is **per-server**: it's bound to each server's own spawn identity
(`plugin::server_spec_hash` over command + url + args), so approving one
server doesn't bless a later-added one, and editing one server's command
re-gates only that server. The connect loop skips each untrusted stdio
server individually; a blanket grant (the env opt-in or a whole-file
approval) still trusts everything and short-circuits the per-server work.

## One ladder, not two (the MCP migration, finished)

MCP was the last consumer and the only one that *gained* behaviour, so it
landed last. It had been carrying a second resolver:

| | ladder | shape |
|---|---|---|
| `read_config_servers` | `scope::plan` + union | MERGES project + user |
| `resolve_config` (gone) | hand-rolled env ▷ project ▷ user | ONE winning file |

Two answers to one question, and they disagreed in the worst direction. The
Plugins pane, `agentty config mcp` and ACP delegation all read the merged
map, so they listed every server. The **connect loop** read the single file —
so the moment a repo had `./.agentty/mcp.json`, every server in
`~/.agentty/mcp.json` silently stopped running while still being listed.
You saw your server; it never ran.

The fix is the union fold everywhere, plus trust moving from a whole-config
verdict to a per-source one:

- `bool project_local`, threaded through four functions plus a convenience
  overload that discarded it (`bool ignore`), is gone. That bool was
  provenance being reconstructed badly when `plan()` already returned it as
  `Source::locus`.
- The locus decision is `trust_of`'s alone. `$AGENTTY_MCP_ALLOW_PROJECT`
  survives as a back-compat pre-check and nothing more — it is blunt (one
  variable trusts every project config forever) where the content hash
  re-gates on any edit.
- An unvouched project config no longer takes the user's own servers down
  with it. Trust is per source, then per server within it.

That last point is what earned `agentty config mcp` its trust column: with
one gate there is one answer to print, and it is computed by calling the
same `trust_of` the spawn path calls.

## Foreign config: import, never a read root

The three tiers of foreignness, by **who owns which part**:

| tier | example | their dir | their leaf | their schema | policy |
|---|---|---|---|---|---|
| native | `.agentty/mcp.json` | no | no | no | read + write |
| dialect | `.claude/skills/` | yes | no | no | read, same product |
| foreign | `~/.junie/mcp/mcp.json` | yes | yes | yes | **adapter** |

A dialect is exactly `<base>/<their-dirname>/<our-leaf>` — one substitution,
in one position. That is why it fits the product. A foreign config swaps the
leaf shape too and often has no project-side counterpart, so forcing it in
would turn `plan()` from a product back into a hand-written list of paths
with tags on them — the exact thing the product was invented to kill.

One rule decides every foreign source:

> **Does acting on this content EXECUTE something?**
> No → read it live; staleness is the only risk.
> Yes → import it; show what you found, write it down, own it.

`mcp.json` spawns processes. Live-reading a file we do not own, cannot
validate, and whose edits we never observe would let another tool's config
change what we execute — silently, because nothing tells us it changed.

### Why import is what #60 actually asked for

[#60](https://github.com/1ay1/agentty/issues/60) asked agentty to *read*
`~/.ai/mcp/mcp.json`, "to have all my custom MCP servers in one place". The
literal ask is a sixth read location; the stated wish is the opposite of it.
If agentty read that file, the servers would live in **two** places with a
precedence rule between them. After `agentty mcp import --from junie` there
is one file, which is what was asked for.

Two things fell out of checking rather than assuming:

- **The path in the title does not exist.** JetBrains documents Junie's
  user-scope config at `~/.junie/mcp/mcp.json` (project: `.junie/mcp/mcp.json`).
  The reporter remembered the shape, not the directory.
- **Which is exactly why `agentty config mcp` prints what it does NOT read**,
  with the command that adopts it. That turns "agentty ignores my servers"
  from an issue someone files into a line they already read.

Import never replaces a name agentty already has (without `--force`):
swapping a command under an approved name is the MCPoison shape arriving
through the front door. A malformed foreign config refuses rather than
importing half of itself.

### Import does not translate — it copies

The entry is adopted **verbatim**, and that is the whole trick. `mcp.json` is
ONE schema shared across tools, so a foreign entry may carry `env`,
`headers`, `timeoutMs`, `connectTimeoutMs` or `tools.exclude` — every one of
which agentty's own connect path already reads.

The first version of this rebuilt the entry from a `ServerSpec`, which models
only what `plugin add` can set (command/args/url/type). That looked principled
and was a data-loss bug: a server with an API key in its env block imported
*looking complete*, then failed at spawn with an auth error naming nothing.
**A lossy import is worse than a refused one** — the failure is silent and
arrives later, detached from its cause.

Copying whole also means a field added to the format next year survives
without a code change, which an allow-list of known keys can never do. The
decoded `command`/`url` on `Found` exist only to display the server and to
reject an entry with no transport at all.

## Where it lives

- `include/agentty/scope/scope.hpp` — the types + the two inline fold
  templates (generic, so they live in the header).
- `src/scope/scope.cpp` — the impure edge: `plan`, `current_env`,
  `trust_of`, `Approvals`.
- `tests/scope_test.cpp` — 11 cases over the pure surface (precedence
  order, the `/`-and-no-HOME guards, both folds + provenance +
  parse-error propagation, the content-bound trust re-gate).

See also [`plugin-model.md`](./plugin-model.md) (the consumer that
motivated this) and the user-facing memory smart-scope note in
[`../website/configuration.md`](../website/configuration.md#memory-scope).
