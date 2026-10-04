# The `.agentty` directory — user and project — design

What exists, what the shape of it should be, and what the gap is. Reference
for anyone adding a file to either locus, or adding a new override.

## 1. The invariant worth stating first

**`.agentty` means the same thing at both loci.** That is the whole reason the
name is reused: a user types `.agentty` and gets the same mental model whether
it hangs off `$HOME` or off a repo root. Breaking that symmetry to solve a
local problem costs more than the problem.

Two layers already encode this and must not disagree:

| layer | question | header |
|---|---|---|
| `agentty::scope` | where do I **read** config from? | `scope/scope.hpp` |
| `agentty::dirs` | where do I **write** bytes to? | `dirs/dirs.hpp` |

`scope` is a precedence lattice (`Locus × Dialect`), `dirs` is a storage
resolver (`Root × Lifecycle`). They are deliberately separate — reading has
precedence and provenance, writing has lifecycle and ownership — and neither
should grow the other's concerns.

## 2. The user root: `~/.agentty`

Settled, documented in `util/user_root.hpp`, and the rationale there is sound.
Summary, because the rest of this doc argues from it:

```
~/.agentty/
  settings.json  mcp.json  hooks.json  skills/  agents/  commands/
                        ← config: top level, hand-editable, discoverable
  threads/  memory.jsonl ← data: conversation history, learned memory
  credentials/           ← secrets (0700 dir, 0600 files)
  cache/                 ← refetchable: models.dev, update stamps
  logs/                  ← diagnostics
```

### Why one root and not XDG's four

Not a style preference. Two concrete arguments, both still correct:

1. **A four-root layout has four answers to "where does this new file go",
   so files land by coin-flip.** This already happened here: `modelsdev.json`
   (a cache) in `.config`, `stderr.log` in `.config` while `agentty.log` sat
   in `~/.agentty`, and a comment in `main.cpp` claiming `~/.agentty` next to
   code writing `config_dir()`.
2. **`~/.config` is what people back up and sync as dotfiles.** This directory
   holds OAuth refresh tokens and full conversation history. Silently syncing
   secrets and hundreds of MB of threads into a dotfiles repo is a real harm.

And `$XDG_CONFIG_HOME/agentty`-for-everything is not the compromise it looks
like — the spec says `.config` is for config, so honouring it properly means
the four-way scatter, not one directory in the config root.

Peers agree: Claude Code (`~/.claude` + `CLAUDE_CONFIG_DIR`), gh, aws, gcloud,
cargo, rustup all use one app dotdir.

### Overrides, and the rule that keeps them from becoming XDG

```
$AGENTTY_HOME         the whole root
$AGENTTY_THREADS_DIR  conversation history
$AGENTTY_CACHE_DIR    refetchable
$AGENTTY_LOGS_DIR     diagnostics
$AGENTTY_RAG_DIR      retrieval indexes — PROJECT-scoped, the odd one out
$AGENTTY_DOCS_DIR     which corpus to index (not where state goes)
```

Note `$AGENTTY_RAG_DIR` in that list. Every other override relocates part of
the **user** root; that one relocates part of the **project** root, and it had
to be invented as a special case precisely because the project root has no
categories to hang it off. §3 is largely about removing that asymmetry.

The distinction from XDG is **the default**. XDG scatters by default; here the
default is one root and an override is a deliberate per-install choice that
changes nothing for anyone who does not set it.

Three semantics, inherited by every override via `dirs::resolve`:

- `$VAR` wins when set and **non-empty** (an exported-but-blank var is a
  common shell accident and must relocate nothing)
- a **relative** value resolves against the root, never the process CWD —
  `AGENTTY_LOGS_DIR=logs2` has to mean one directory, not a different one per
  launch directory
- a failed override **warns once and falls back**; silently serving the
  default tells the user the wrong thing twice

**Deliberately not overridable:** `credentials/` and `settings.json`. Small,
easy to lose track of, and a secret that moves because of a line in a shell
profile is a secret nobody can find later.

### What decides whether something gets an override

Measured, not guessed. On this install, right now:

| | |
|---|---|
| `settings.json` | 4 KB |
| `credentials/` | 20 KB |
| `logs/` | 55 MB |
| `threads/` | 763 MB |

Four orders of magnitude between the two groups, and the split is not
arbitrary: the large ones **grow with use** and the small ones do not. An
override exists for a category that grows; a category that is small and
sensitive does not get one.

(The user-root header quotes 4 MB of threads and 38 MB of logs from when it
was written. Both have since grown ~20x and ~1.5x. That the *ratio* argument
survived the growth is the point — but the numbers in prose go stale, which is
why the rule is "does it grow", not "is it over N MB".)

## 3. The project root: `<project>/.agentty`

Here is the gap. Today, in this repo:

```
.agentty/
  skills/                          config, hand-authored, committed
  memory.jsonl                     data, append-only, arguably committed
  rag_code.<tag>.ragdb             derived, 31 MB, never committed
  rag_docs.<tag>.ragdb             derived, 7 MB, never committed
  rag_feedback.tsv                 data, accumulated, not regenerable
  routing_memory.tsv               data, accumulated
  routing_memory.tsv.lock          runtime, process-scoped
  *.meta.json                      derived sidecars
```

**Thirteen entries, one flat directory, four different lifecycles.** The user
root sorted itself into `cache/ credentials/ logs/ threads/` for exactly this
reason and the project root never did. It is the coin-flip layout the user-root
rationale rejects — on the other side of the same name.

### The four lifecycles, named

| class | examples | committed? | safe to delete? |
|---|---|---|---|
| **config** | `skills/`, `mcp.json`, `hooks.json` | yes — the team wants it | no |
| **local config** | personal overrides | **no** — gitignored | no |
| **data** | `memory.jsonl`, `rag_feedback.tsv`, `routing_memory.tsv` | judgement call | no — accumulated from use |
| **derived** | `*.ragdb`, `*.meta.json` | no | **yes** — rebuilds |
| **runtime** | `*.lock` | no | yes — process-scoped |

Two properties fall out, and they are the ones a flat directory cannot express:

- **You cannot write a correct `.gitignore` rule.** `.agentty/` ignores the
  skills the team wants committed. `!.agentty/skills/` plus a growing deny
  list is a rule nobody maintains correctly. The categories need to be
  directories so the rule can be structural.
- **"Reset the derived state" has no safe spelling.** `rm -rf .agentty`
  deletes hand-authored skills and accumulated feedback alongside the 38 MB of
  rebuildable index.

### How peers solve it

**Claude Code** splits by *who owns it*, same directory, two files:

```
.claude/settings.json        committed  — team policy
.claude/settings.local.json  gitignored — machine-specific
```

Simple and effective, and notably it does **not** solve the derived-data
problem — Claude Code keeps per-project state in `~/.claude.json` instead,
which is the other valid answer (push it to the user root entirely).

**Cargo** splits by *lifecycle*: one `target/` holding everything derived, and
the self-ignoring trick — a `.gitignore` containing `*` written **inside**
`target/`, so the directory excludes itself and the repo root's `.gitignore`
stays clean. That is the better idea for derived data, because it needs no
cooperation from the repo.

**VS Code / JetBrains** split by *committed intent*, with the same
committed-vs-local distinction and the same recurring confusion about which
file to edit — the lesson being that the split only works if the naming makes
the answer obvious.

### The proposal

Mirror the user root, because the invariant in §1 says both loci should read
the same:

```
<project>/.agentty/
  settings.json  mcp.json  hooks.json  skills/  agents/  commands/
                        ← config: committed, the team's
  settings.local.json   ← local config: gitignored, yours
  memory.jsonl          ← data
  state/                ← data: rag_feedback.tsv, routing_memory.tsv
  cache/                ← derived: *.ragdb, *.meta.json, *.lock
    .gitignore          ← containing `*` — self-ignoring, cargo's trick
```

What this buys:

- **`cache/` is deletable by definition.** "Reset the derived state" becomes
  `rm -rf .agentty/cache`, and nothing hand-authored is at risk.
- **The ignore rule is structural**, not a deny list. `cache/` ignores itself;
  the only repo-root rule needed is `.agentty/settings.local.json`.
- **`$AGENTTY_RAG_DIR` stops being a special case.** It exists today because
  the indexes are the one category big enough to need relocating, and they sit
  in a directory with no category. Under `cache/`, the generic
  `Root::Project` + `Lifecycle{rebuildable}` already covers it.
- **It matches the user root**, so `.agentty` means one thing.

Not proposed: pushing project state into `~/.agentty` keyed by path (the
Claude Code `~/.claude.json` answer). It keeps the repo clean but makes the
state invisible, un-inspectable, and impossible to delete per project without
knowing the hashing scheme. The whole appeal of a project dotdir is that it is
*there*.

## 4. Adding a file: the decision procedure

Three questions, in order. They are answerable without reading any other doc,
which is the point.

**1. Does it only make sense because a model is on the other end?**
Yes → the tool boundary (`tools::util`). No → app-wide (`util`). *(That split
has its own note in `tool/util/fs_helpers.hpp` and a build-time lint.)*

**2. Does it follow the HUMAN or the CODE?**
Human → `Root::User`. Identical regardless of which checkout you stand in:
settings, credentials, threads, learned memory.
Code → `Root::Project`. Meaningless away from the tree that produced it, and
must not be shared between unrelated checkouts: retrieval indexes, project
memory, routing feedback.

**3. What is its lifecycle?** This picks the subdirectory, and it is the
question the project root currently cannot answer:

```
config    hand-authored, committed        → top level
local     hand-authored, gitignored       → *.local.json
data      accumulated, not regenerable    → state/ (or top level if small)
derived   rebuildable                     → cache/
runtime   process-scoped                  → cache/
```

Then: does it **grow without bound**? If yes it needs an override, and the
override goes through `dirs::Spec` so the three semantics in §2 come for free.
If no, it does not get one — every override is a thing that can be set wrong.

## 5. Where this is enforced, and where it is not

Enforced today:

- `dirs::Spec` makes lifecycle a **field**, so adding a store forces the
  retention decision rather than defaulting to "forever" (this is what the
  47 MB orphaned `.ragdb` cost us before `#62`).
- `scope::plan` generates the precedence ladder once, so no feature
  re-hardcodes it.
- `tests/lint/util_layering.cmake` makes the one-way `util` dependency a build
  error.
- `Source` carries provenance as a value, so an edit targets the file a value
  actually came from — structurally killing the "toggled a project server,
  silently edited the user file" bug.

Not enforced, and the honest list:

- **Nothing stops a flat write to the project root.** Every file in §3 got
  there legitimately, one commit at a time. A lint asserting that
  `<project>/.agentty` contains only known top-level names would have caught
  the drift.
- **`Locus::Local` is in the lattice but emits no sources.** It was shipped as
  a value and left unwired until a second consumer appeared. `settings.local.json`
  is that consumer.
- **No migration path for the reshuffle.** Moving `*.ragdb` into `cache/`
  needs an adopt-or-orphan decision, same as `#63` step 2.

## 6. Order to do it in

Each step is independently useful, which is the test of whether the
decomposition is right:

1. **A lint on the project root's top level.** Stops the drift before fixing
   it, and is the cheapest thing here.
2. **`cache/` + the self-ignoring `.gitignore`.** Biggest win: makes derived
   state deletable and the ignore rule structural. Needs the adopt-or-orphan
   call.
3. **`settings.local.json` and wiring `Locus::Local`.** The precedence slot
   already exists; this gives it its first real consumer.
4. **`state/`.** Last, because `rag_feedback.tsv` and `routing_memory.tsv` are
   small and the flat layout does not actively hurt yet.

## The variable budget: one anchor, and an honest gap

The storage model should be sayable in one breath, and that is a requirement,
not a boast:

| root | follows | anchor |
|---|---|---|
| `~/.agentty` | the HUMAN | `$AGENTTY_HOME` |
| `<project>/.agentty` | the CODE | **none yet** |

That second row is the remaining gap, and `agentty config env` prints it that
way rather than papering over it. `$AGENTTY_RAG_DIR` relocates the retrieval
indexes and the feedback TSV and *nothing else* — `memory.jsonl` stays at
`<project>/.agentty` regardless — so calling it the project anchor would be a
confident wrong answer. The user root has categories to hang an override off;
the project root is flat, which is exactly why `RAG_DIR` had to be invented as
a one-off instead of falling out of `(Root, Lifecycle)`.

Seven variables exist; `$AGENTTY_MCP_CONFIG` is not storage at all (it names
one file to *read* — scope's Explicit locus, a different axis). The rest each
move ONE leaf. `$AGENTTY_THREADS_DIR`, `$AGENTTY_CACHE_DIR` and
`$AGENTTY_LOGS_DIR` predate `$AGENTTY_HOME` and each move a leaf it already
moves; they stay because removing a released variable breaks a working setup
with no error message.

**Nothing new is added on that axis.** A new storage category is a leaf under
an existing root. The pressure to add one is constant and the rule from
`user_root.hpp` is the answer: a category earns a variable only if it grows
unboundedly AND you would plausibly put it on a different device from its
siblings.

## The project root ignores itself

`<project>/.agentty` holds derived state — 33 MB of retrieval index in this
repo — directly inside a user's source tree. agentty's own `.gitignore` lists
it, which protects agentty's repo and nobody else's: in any other checkout the
first `git add -A` after a warm index stages tens of megabytes of binary, and
the author finds out at review time.

So `dirs::resolve` writes a `.gitignore` *inside* the directory when it
creates it, containing `*`. The rule ships with the thing it protects, which
makes it structural rather than folklore every user has to know. It is also
the only file in there git can see.

Two properties, both load-bearing:

- **Never overwritten.** A user who deliberately tracks part of `.agentty` (a
  shared `skills/` directory is a real use) edits the file, and agentty leaves
  it alone forever after. A tool that rewrote it every launch would be a tool
  that argues.
- **Project root only.** `~/.agentty` is not inside a repository, so the same
  file there would be noise.

## Seeing it: `agentty config`

Six read locations and two write roots are fine when you can see them. Six
invisible ones are the problem — that is what #58 and #60 are really about,
and both are the same person asking twice for *fewer things to track*.

```
agentty config          every concern, one line each
agentty config mcp      the full ladder, including what is NOT read
agentty config env      the storage model above
```

The design constraint is that it **cannot lie**. A tool describing the
program is worthless if it can drift from it, so
`include/agentty/config/inventory.hpp` *owns* every `Layout` and `Spec`, and
the features consume them:

```
config::kMcpLayout  ──┬──>  bridge.cpp reads servers with it
                      └──>  `agentty config mcp` prints it
```

One constant, two readers, no second copy to disagree. `config_inventory_test`
enforces it mechanically: no file outside the inventory may brace-init either
type, and the three user-root specs must resolve byte-identically to the
accessors that shipped first.

The registry lives above both primitives, never inside them — `scope` and
`dirs` still know nothing about their callers, which is what keeps them
reusable. The dependency points one way.

The most useful half of the output is **"not read, and why"**. It is derived
as the set difference between the full dialect product and the feature's own
`dialects`, so it cannot disagree with what *is* read, and it answers #60
before anyone files it.

## See also

- `include/agentty/util/user_root.hpp` — the one-root argument, at length
- `include/agentty/dirs/dirs.hpp` — the write-side resolver
- [`scope-model.md`](./scope-model.md) — the read-side algebra
- [`data-dirs.md`](./data-dirs.md) — why `store` became `dirs`, and the
  `project_anchor()` vs `project_root()` distinction
