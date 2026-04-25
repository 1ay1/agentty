# Persistent codebase memory

moha's memory model — what gets cached, where it lives, how it
travels into the system prompt, and how it stays fresh.

## Goals

1. **Free at steady state.** Anthropic's prompt cache reuses identical
   prefix bytes at ~10% cost. If the cached system block is stable
   across turns, accumulated knowledge effectively costs nothing per
   subsequent turn.
2. **Self-curating.** Memory accumulates from real work — investigate
   runs, file reads, commit history mining — without the user needing
   to maintain `CLAUDE.md`-style docs by hand.
3. **Self-invalidating.** Knowledge tied to file mtimes and git HEAD
   so we never feed the model a memo that's been contradicted by
   subsequent changes.
4. **Curatable.** The model AND the user can `remember` / `forget` /
   `recall` explicitly. Memory is not magic.
5. **Scales to huge codebases.** Module-level abstraction +
   `navigate` semantic search keeps the system-prompt budget tight
   even at 100k files.

## Layers

The memory model is five orthogonal layers. Each can be disabled
without breaking the others.

### 1. Symbol index (`moha::index::RepoIndex`)

In-memory index of every defined symbol across the workspace. Built
lazily on first query, refreshed when a file's mtime changes. Per-
language regex extractors (no tree-sitter dep) cover C/C++, Python,
JS/TS/JSX/TSX, Go, Rust.

Beyond the symbol → file map, the same scan also computes:
- **Centrality**: cross-file mention counts (PageRank-flavoured —
  one iteration of "how many other files reference this symbol").
- **Per-file score**: sum of contained-symbol centrality, with a
  recency boost (×2 for files modified in the last hour).
- **Reference graph**: bidirectional file ↔ symbol edges, exposed as
  `files_using(name)` and `symbols_used_by(path)`. Powers the
  `find_usages` tool.

### 2. File knowledge cards (`moha::memory::FileCardStore`)

One LLM-distilled paragraph per file, generated **async** by a
background Haiku worker. Triggered when `investigate`'s sub-agent
touches a file. Cached on disk under `<workspace>/.moha/cards/<sha>.json`,
keyed by file mtime. Capped at 256 cards (LRU eviction).

Per card: `{path, mtime, summary (~80 words), source, confidence,
generated_at}`. Surfaced inline in the repo map as `// <summary>`
continuation rows under each file.

### 3. Memos (`moha::memory::MemoStore`)

Q-A pairs from `investigate` runs and `remember` calls. Persisted to
`<workspace>/.moha/memos.json` with provenance fields:
- `model` — which model wrote it (drives confidence multiplier)
- `source` — `"auto"` (investigate) / `"manual"` (remember) /
  `"adr"` (mined from git) / `"long_term"` (distilled)
- `base_score` — 0–100 set by the writer
- `git_head` — SHA at creation
- `file_refs` — workspace-relative paths the memo discusses

`Memo::effective_confidence(workspace)` combines base score, model
multiplier (Opus 1.0 / Sonnet 0.85 / Haiku 0.7), file-mtime
freshness check, and 30-day linear age decay.

The `compose_prompt_block` renderer ranks memos by recency and
applies tier-based rendering:
- `≥70%` confidence → full synthesis body verbatim
- `40–69%` → first paragraph + `[verify before acting]` hint
- `<40%` → topic only + `[recall for full text]` hint

Capped at 8 KB total / 1.5 KB per memo. Hard memo cap at 64 (FIFO
eviction). Atomic write via `tmp + rename` with stderr error
surfacing.

### 4. ADRs (architectural decisions)

`mine_adrs(since)` walks `git log` for the configured window
(default 180 days, 200 commits), filters to commits with bodies
≥80 chars, asks Haiku to triage each: `SKIP` or `DECISION: …  WHY: …`.
Decisions become memos with `source="adr"`, the commit SHA as id
(idempotent dedup), and a base score of 75.

After one mine, the agent inherits the project's design history
without anyone having to re-document it.

### 5. Hot files (`moha::memory::HotFiles`)

Recent-activity index built from `git log --since` ∪ filesystem mtime
scan. Three buckets: last 60 min / 24 hr / 7 days. Cached for 60 s.
Injected as `<recent-activity>` in the system prompt so the agent
focuses on what the user is currently touching.

## System prompt layout

Every request's cached system prefix carries:

```
<repo-map>
  Module overview (top areas of the codebase by importance)
</repo-map>

<recent-activity>
  Files touched in last 60 min / 24 hr / 7 days
</recent-activity>

<learned-about-this-workspace>
  *** READ THIS FIRST ***
  Memos with confidence-tiered rendering
</learned-about-this-workspace>
```

All three live INSIDE the cached system block (same `cache_control`
breakpoint). The first turn after a memo update pays the
re-tokenisation cost; subsequent turns are cache hits.

## Tools

```
investigate(query, model?)       sub-agent that returns one synthesis + saves a memo
remember(topic, content, files?) explicit memo save (high base score, source="manual")
forget(target)                   drop a memo by id or topic substring
memos(filter?)                   list memos with confidence + freshness markers
recall(topic)                    fetch the FULL text of a memo (bypasses prompt budget)
mine_adrs(since?, max_commits?)  walk git log, distill decisions into ADR memos
find_usages(symbol)              "who uses Foo?" via the reference graph (microseconds)
navigate(question)               semantic finder: ranked files / symbols / modules / memos
outline(path)                    one file's symbol map (kind-grouped)
repo_map(path?, max_kb?)         hierarchical map; drill-down with path arg
signatures(pattern, limit?)      cross-file symbol grep with kind tags
```

## Big-codebase strategy

At 10k+ files the file is the wrong unit. `RepoIndex::detect_modules`
groups by parent directory truncated to 4 components from workspace
root, requires ≥2 source files per module, sorts by score.

`hierarchical_map` becomes the default repo-map output:
```
[12847] src/runtime/   (28 files; top: Model, update, Cmd, ...)
[ 9233] include/moha/tool/   (14 files; top: ToolDef, ToolError, ...)
[ 8104] src/io/   (6 files; top: Client, Request, HttpError, ...)
```

Same 4 KB budget, but the unit is "module description" not "file
name" — fits a 100k-file repo's *map* in the same space that used
to show 80 alphabetical filenames.

`navigate(question)` is the killer entry point: pure-C++ scoring
(token overlap × 10 + substring × 4 + centrality cap 50) over
symbols + files + cards + memos + modules, returns ranked
candidates in <100 ms. The agent calls `navigate` first, `outline`
or `read` only on the candidates that look promising.

## Persistence layout

```
<workspace>/.moha/
  memos.json              # MemoStore — versioned, atomic writes
  cards/
    <hash>.json           # one FileCard per file
```

Both directories are safe to gitignore (per-developer memory) OR
commit (shared team knowledge). `.moha/cards/` regenerates from
files on first use; `memos.json` is irreplaceable, so commit it if
you want team-wide knowledge accumulation.

## What's deliberately NOT in the model

- **Local embeddings**: dependency-heavy; the structural signals
  (centrality + name match + cross-references) cover 90% of the use
  case at zero cost.
- **Cross-workspace memos**: tempting but premature.
- **Auto-distillation triggered from `MemoStore::add`**: would
  deadlock the store's mutex (the LLM call is async, can't run
  inline). Deferred to a future maintenance tool.
- **Conflict detection**: handled by `forget` + manual curation.
