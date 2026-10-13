# Storage roots — where agentty's bytes live — design

## The problem this fixes

`agentty::scope` made config *lookup* total: one algebra, `Locus × Dialect`,
provenance on every hit. It answers **"where do I READ this from?"**

Nothing answers **"where do I WRITE this to?"** — and that half grew the
same way config did before scope: one decision at a time, at each call site.

Three open issues are the same missing abstraction seen from three angles:

- **#58** — "store all non-configuration data on a different path". Partly
  shipped: `$AGENTTY_THREADS_DIR`, `$AGENTTY_CACHE_DIR`, `$AGENTTY_LOGS_DIR`
  move the bulk of the *user* root.
- **#61** — RAG indexes aren't relocatable. 82 MB/project, pinned to
  `<cwd>/.agentty`, reachable by no variable at all.
- **#62** — stale `.ragdb` files are never reclaimed. A 47 MB orphan from a
  naming scheme no current build can read.

They look like a feature request, a gap and a leak. They are one hole:
**there is no storage primitive on the project side, and no lifecycle on
either side.**

## What exists today

Two roots, with very different maturity.

**User root** (`util/user_root.hpp`) is in good shape. `resolve_subdir()`
already encodes the rules worth having:

```cpp
fs::path resolve_subdir(const char* env, const char* leaf, bool owner_only);
```

- `$env` wins when set and non-empty; empty ⇒ unset (an exported-but-blank
  var relocates nothing).
- A **relative** override resolves against the root, never the CWD — so
  `AGENTTY_LOGS_DIR=logs2` means one directory, not a different one per
  launch directory.
- A failed override **warns and falls back** rather than silently papering
  over itself: "the setting didn't take, and nothing said so" is being
  wrong twice.
- `owner_only` forces 0700 where the default's inherited mode isn't enough.

**Project root** has no equivalent. Each writer re-derives its own:

| Consumer | Resolves via | Overridable | Reclaimed |
|---|---|---|---|
| threads, cache, logs | `resolve_subdir()` | yes | logs rotate |
| credentials | user root, 0700 | no (deliberate) | n/a |
| `memory.jsonl` (user) | user root | no | append-only |
| `memory.jsonl` (project) | `project_root()` + writability clamp | no | append-only |
| `rag_{docs,code}.ragdb` | `fs::current_path() / ".agentty"` | **no** | **never** |
| `rag_feedback.tsv` | `fs::current_path() / ".agentty"` | **no** | never |
| skills / agents / commands | `scope::plan`, `project_root = "."` | n/a (read) | n/a |

Two defects fall straight out of that table.

**1. Project resolution is inconsistent.** Memory clamps to
`project_root()`; RAG uses raw `fs::current_path()`; skills deliberately
passes `"."`. `src/tool/skills.cpp` carries the note:

```cpp
// NOTE: project stays cwd-relative here (unlike memory's project_root()
// clamp) — skills has always resolved ".agentty/skills" against cwd, so
// env.project_root = "." preserves that exactly.
```

A note explaining why one consumer disagrees with another is the shape of a
missing primitive. The user-visible consequence: `cd src/ && agentty` gets a
**second** 82 MB index under `src/.agentty`, because `current_path()` moved
and nothing anchored it.

Worth being precise about why, because it bears on the fix.
`project_root()` is documented as *"the process cwd, clamped inside the
access boundary"* — it does **not** walk up to find a project marker. So
memory's clamp buys containment (never `/`, never outside the workspace)
but not *stability*: launch from a subdirectory and `project_root()` moves
too. Routing RAG through `project_root()` therefore fixes the
`--workspace /` case and the `/` sentinel case, and does **not** on its own
fix the duplicate index.

Stable per-project storage needs an anchor that doesn't move with the cwd:
the nearest enclosing directory containing a project marker (`.git`,
`.agentty`, `.hg`, `.svn`), falling back to `project_root()` when there is
none. That's a separate function from `project_root()` — relative tool
paths genuinely should resolve against the cwd, which is what
`project_root()` is for. Storage wants a different question answered.

**2. Nothing has a lifecycle.** `resolve_subdir` answers *where*. No
function answers *for how long*. `.ragdb` files are the proof — the embedder
tag in the filename is correct (it prevents serving vectors from an
incompatible space) but it makes filenames **unbounded in number**, and
nothing collects the losers.

## The design

One primitive per root, and a lifecycle declared next to the path.

### Store: the project-side peer of `resolve_subdir`

```cpp
namespace agentty::store {

// WHICH root a thing belongs under. Not "where" — that's the resolver's job.
enum class Root {
    User,     // ~/.agentty           — follows the human
    Project,  // <project>/.agentty   — follows the code
};

// How long a thing is allowed to live. Declared WITH the path, so adding a
// store forces the decision instead of defaulting to "forever".
struct Lifecycle {
    bool rebuildable   = false;  // safe to delete; worst case it regenerates
    unsigned keep_last = 0;      // >0: keep N newest variants, sweep the rest
    std::chrono::seconds min_age{0};  // grace window before anything is swept
};

struct Spec {
    Root             root;
    std::string_view leaf;       // "rag", "threads", "memory.jsonl"
    std::string_view env;        // override var; empty ⇒ not overridable
    bool             owner_only = false;
    Lifecycle        life{};
};

// Total: a Spec always yields a path or a typed error. Never an empty path
// that each caller re-interprets.
[[nodiscard]] std::expected<fs::path, Error> dir(const Spec&);

}  // namespace agentty::store
```

Three properties carried over from `scope`, because they're why `scope`
worked:

- **The feature owns its identity.** `store` holds no enum of features and
  no filenames. A caller hands it a `Spec`, exactly as it hands `scope` a
  `Layout`. Otherwise this primitive becomes a registry of everything
  downstream and the dependency inverts.
- **Total, not partial.** `std::expected`, never a silent empty path.
- **Provenance.** The resolved path records whether it came from an
  override, the default, or a fallback-after-warning — so the Storage pane
  can show where the bytes actually are, which is the same reason `scope`
  tracks `Source`.

### Project root: one answer

`Root::Project` resolves through a **storage anchor**, not the raw cwd:

1. the nearest enclosing dir holding a project marker (`.git`, `.agentty`,
   `.hg`, `.svn`), searching upward from `project_root()`;
2. else `project_root()` itself;
3. in both cases subject to `scope`'s `usable_project_root()` guard —
   reject `/` and `C:\`, since the unrestricted-access sentinel is never a
   place to scatter state.

Step 1 is what actually fixes the duplicate index, and it's why this can't
just call `project_root()`. Step 3 is the containment property memory
already has, generalised.

This deliberately does not change `project_root()`. Relative tool paths
*should* track the cwd — `read src/foo.cpp` must land where the user is
looking. Storage asks a different question and gets its own answer; the
header should say so, so nobody later "unifies" them and reintroduces
either bug.

Skills keeps cwd-relative *read* semantics via `scope::Env.project_root`;
this is about writes, so there's no conflict — also worth stating in the
header for the same reason.

### Overrides

Reuse `resolve_subdir`'s rules verbatim, including for the project root.
`$AGENTTY_RAG_DIR` is the #61 fix, with the one wrinkle a central location
creates: if project indexes live under a shared root, the leaf needs a
per-project discriminator, since today the path is disambiguated by *being*
in the project.

```
$AGENTTY_RAG_DIR/<basename>-<hash-of-project-path>/rag_code.<tag>.ragdb
```

Hash of the absolute project path, basename retained for human
readability — the same reasoning as the embedder tag: machine-identity for
correctness, a readable prefix so a human can tell what they're looking at.

**Rejected:** routing RAG under `$AGENTTY_CACHE_DIR`. It's one fewer
variable and plausibly what a user expects, but it silently changes the
meaning of a shipped var from "user-root cache" to "also per-project data",
and someone who set it in 0.9.19 to move 5 MB of user cache would silently
acquire 82 MB/project. A new var is additive; redefining a released one
isn't.

### Lifecycle, and why it belongs here

`Lifecycle` sits in the `Spec` so that **adding a store forces the
retention decision**. That's the structural fix for #62: the leak wasn't an
oversight in the RAG code, it's that nothing in the codebase ever asked.

`agentty::blobs::gc` is the precedent and the shape to copy — mark-and-sweep,
`dry_run`, `min_age` grace, once-per-day stamp, and the rule that if it
can't read something it deletes nothing. The RAG case is strictly easier:
reachability is just "is this the tag the live config produces".

But it should be *less* cautious than blob GC, and the reason is the
asymmetry `rebuildable` names. Deleting a blob destroys history that cannot
be recovered. Deleting a `.ragdb` costs one re-index. `blobs::gc` is
careful because being wrong is unrecoverable; RAG GC can be aggressive
because being wrong is slow. Same mechanism, opposite default — which is
exactly the kind of thing a shared primitive gets wrong unless the
distinction is a field.

`keep_last = 1` for RAG, so an A/B between two embedders doesn't force a
full rebuild each time you switch back. The untagged legacy
`rag_code.ragdb` is deleted outright: no current code path can read it.

## What this closes

- **#61** — `$AGENTTY_RAG_DIR`, plus the discriminator that makes a shared
  root safe.
- **#62** — `Lifecycle` + a sweep on successful persist. `keep_last=1`,
  legacy untagged files dropped.
- **#58** — completes the answer. "Non-configuration data" becomes
  enumerable rather than a list someone maintains by memory, and the
  Storage pane can show every root, its override, and its actual size.

And the latent one nobody filed: `cd src/ && agentty` currently builds a
second 82 MB index. The storage anchor fixes that — note this needs the
marker search, not just `Root::Project`, for the reason above.

## Scope of the change

Deliberately not a rewrite. `resolve_subdir` already has the right
semantics; this generalises it to a second root, adds the lifecycle field
it never had, and ports consumers one at a time — the same adopt-smallest-first
discipline `scope` used (memory first, then the rest).

Order:

1. `store::dir()` wrapping today's `resolve_subdir` behaviour; user-root
   consumers keep working unchanged.
2. The storage anchor + `Root::Project`. Fixes the `cd src/` duplicate and
   lands `$AGENTTY_RAG_DIR`. Needs a migration read: an existing
   `<cwd>/.agentty/*.ragdb` should be adopted rather than orphaned, or
   step 2 silently strands the very files step 3 then deletes.
3. `Lifecycle` + sweep; RAG is the first consumer with a non-trivial one.
4. Storage pane reads the Spec registry rather than a hand-kept list.

Steps 2 and 3 are independently shippable and each closes an issue on its
own.

## Later: every directory overridable

The table above said credentials were "not overridable (deliberate)", on the
idea that a secret which moves is a secret nobody finds. In practice people
want the opposite: tokens on an encrypted volume, threads on a big disk,
logs in tmpfs. Every directory now has `AGENTTY_<NAME>_DIR`, and
`agentty config` always prints where each one went, which answers "where is
my secret". Storage is set by environment only: the app never writes it, so
the TUI can't move your data by accident. `owner_only` still forces
0700 on credentials and threads wherever they land.

## See also

- [Storage](../website/storage.md): the user-facing map of every file.
- [`scope-model.md`](./scope-model.md) — the config-*lookup* algebra this
  mirrors. Read it first; the `Layout`/`Spec` symmetry is intentional.
- `include/agentty/util/user_root.hpp` — the long note on why ONE root and
  not XDG's four. That argument is why `Root` has two values and not five.
- `include/agentty/io/blob_gc.hpp` — the GC precedent, and the cautious end
  of the `rebuildable` axis.
