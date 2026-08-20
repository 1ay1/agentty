---
name: text-transforms
description: >-
  Use when a task is FIND-then-reshape or FIND-then-tally rather than just
  find or just edit: extracting a value from every match (imports, routes,
  TODO owners), counting/grouping matches across a codebase, a project-wide
  string/regex rename, or reading one concern out of a huge file. Covers the
  extract, aggregate, replace and read_filter tools — the sed/awk/`rg -o`/
  `sort | uniq -c` layer that sits between grep (find) and edit (change).
---

# Text-transform & aggregate tools

Four tools turn a *match set* into a **reshaped, reduced** result in ONE call,
so you stop doing grep → read → hand-aggregate over several turns. Reach for
them the moment the question is "what are all the X" or "how many / which files
X" or "rename X→Y everywhere" — not "where is X" (that's `grep`) and not "change
this one spot" (that's `edit`).

## Pick the right tool

| You want to… | Tool | Not this |
|---|---|---|
| Pull a **value** out of every match (capture group or field) | `extract` | grep→read |
| **Count / group / sum** matches across files | `aggregate` | grep + eyeballing |
| **Rename** a literal/regex across many files | `replace` | many `edit`s |
| Read **one concern** out of a big file | `read_filter` | reading it whole |
| Find where something *is* | `grep` | — |
| Change a code *shape* (calls, control flow) | `rewrite_structural` | `replace` |
| Change **one** spot | `edit` | `replace` |

## extract — project each match to a value

Regex + `group` emits that capture per match; `delimiter`+`column` slices a
field out of each matching line (awk `$N`). Add `unique`, `count`
(value→frequency, desc), `sort`, `with_location` (file:line).

- Every import target: `pattern:"import \\{ (\\w+) \\}"  group:1  unique:true`
- Field mode: `pattern:"score"  column:2` (2nd whitespace field of each hit)
- `count:true` gives you `sort | uniq -c` directly.

## aggregate — group matches and reduce

`by`: **file** (which files touch X, and how often — heat-map a refactor),
**match** (tally distinct matched strings), or **capture** (group on a regex
group). `op`: **count** (default), **list** (count + up to 5 sample lines),
**sum** (parse each matched text as a number and total it). Sorted by
magnitude, so the dominant group is first.

- TODO owners ranked: `pattern:"TODO\\((\\w+)\\)"  by:capture  group:1`
- Include heat-map: `pattern:"#include <(\\w+)"  by:capture  group:1`
- Files by match density: `pattern:"unwrap\\(\\)"  by:file`

## replace — project-wide find/replace, DRY-RUN first

**Defaults to a dry run**: returns a per-file before/after preview + total hit
count WITHOUT touching disk. Re-run with `apply:true` to write. Set `regex:true`
for pattern mode (`$1`..`$9`/`$&` in the replacement). **Always pass a `glob`**
to bound the blast radius.

- `find:"OldName"  replacement:"NewName"  glob:"src/**/*.ts"` → preview →
  add `apply:true` once the preview looks right.
- Prefer `rewrite_structural` for code-shape changes (call sites, arg swaps);
  prefer `edit` for a single file. `replace` is for a flat string/regex swap
  spread across many files.

**Safety:** every write lands in the real worktree, so a `replace apply` is
captured by the turn's checkpoint and fully reverted by rewind — same as
`edit`. Still, dry-run first: the preview is free.

## read_filter — condensed read of a big file

Keep only lines matching `pattern` (+`context`), collapsing every gap to
`⋯ N lines ⋯`. Read a 3k-line file at a fraction of the context cost when you
care about one thing. `invert:true` keeps NON-matching lines (grep -v).

- Every error path: `path:"big.go"  pattern:"return .*err"  context:1`
- All route registrations, all `TODO`s, all `panic(`s — one concern, whole file.

## Rules of thumb

- These are **read-only except `replace`** (which is WriteFs and permission-
  gated like `edit`). Use them freely to investigate.
- One `aggregate`/`extract` call replaces a grep followed by manual counting —
  prefer it whenever you'd otherwise scroll a big grep and tally by eye.
- All four honour a `glob` and skip generated/binary/oversized files, same as
  `grep`. Scans are capped; narrow with `glob` on a huge tree.
