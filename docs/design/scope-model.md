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
Layout     { leaf, explicit_env }         (what the FEATURE stores — passed in)
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
