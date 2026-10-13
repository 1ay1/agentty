---
title: Agent Skills
description: Teach agentty your codebase once with a SKILL.md — it's live the next turn. Compatible with Claude Code's skills format.
nav_section: Advanced
nav_order: 40
slug: skills
---

A skill is a folder with a `SKILL.md` that teaches agentty your conventions, DSLs, and tribal knowledge. Drop one in and it's live the next turn — no rebuild, no restart.

## Why skills

On codebases with internal DSLs or house conventions, curated skills push agent accuracy from roughly 20% to 85%. The model discovers a skill by its name and summary, then loads the full instructions only when the task calls for it — so a big library of skills costs almost nothing per turn.

## Writing a skill

A `SKILL.md` is Markdown with a small YAML frontmatter block. The directory name is the skill's slug.

```text
---
name: house-style
description: This project's commit + code conventions. Load before committing or refactoring.
---

# House style

- Commit messages are single-line, imperative, no AI attribution.
- Prefer `edit` over rewriting whole files.
- Run `cmake --build build -j` to build; tests live in tests/.
```

The `description` is what the model sees in the catalog — write it so it's obvious *when* to load the skill. The body is only pulled into context on demand.

Skills are also **searchable**: they're indexed into agentty's [retrieval engine](/docs/retrieval) by default, so `search_docs` (and the proactive pre-turn retrieval) can surface a relevant skill even before the model thinks to load it. Turn this off with `AGENTTY_RAG_SKILLS=0`.

## Where skills live

agentty scans these roots for `<name>/SKILL.md`. Earlier roots win when two skills share a name (project shadows personal; native `.agentty` shadows the interop dirs):

| Location | Scope |
|---|---|
| `<project>/.agentty/skills/` | This repo (native) |
| `<project>/.agents/skills/` | This repo (shared agents format) |
| `<project>/.claude/skills/` | This repo (Claude Code compat) |
| `~/.agentty/skills/` | Every project (native, personal) |
| `~/.agents/skills/` | Every project (shared agents format) |
| `~/.claude/skills/` | Every project (Claude Code compat) |

## Nesting skills (group folders)

A skill directory may sit **up to four levels** below a discovery root. Folders above the skill are group folders — they organize the library and are invisible to the model. The skill's name is its path below the root, lowercased with every character outside `a-z0-9` replaced by `-`:

```text
.agentty/skills/
  pdf-extract/SKILL.md          → name: pdf-extract
  embedded/
    startup/SKILL.md            → name: embedded-startup
  perf/
    alloc/
      pools/SKILL.md            → name: perf-alloc-pools
  My Group/
    Sub_Dir/SKILL.md            → name: my-group-sub-dir
```

The name is what you type as `/name`, so it is held to the spec's charset rather than passed through: spaces, underscores and capitals are folded, runs of separators collapse, and a name that would exceed 64 characters is truncated. A group folder whose name has no `a-z0-9` characters at all is skipped rather than given an invented name.

Flat single-level layouts keep their existing names unchanged. On a name collision the **shallower** skill wins within a root — so an existing flat `a-b/` keeps its name even if an unrelated `a/b/` group folder appears beside it — and project skills still shadow user skills (see the table above). Hidden directories (leading `.`) are never descended into — `.git`, `.obsidian` and friends are storage, not skill territory.

:::note
The four-level cap and the 64-character name limit exist so a skills root that happens to contain a large tree (a stray checkout, a `node_modules`) cannot turn discovery into an unbounded filesystem walk on every cache miss.
:::

:::note
Keep the frontmatter `name:` equal to the joined path slug — `agentty skills` warns on a mismatch.
:::

## Bundled resources

A skill folder can ship supporting files — scripts, reference docs, templates — alongside its `SKILL.md`. agentty enumerates them (bounded to a shallow depth) and read-allowlists the skill directory, so the model can fetch a bundled reference even when it lives outside the workspace boundary. Those reads are read-only; the write gate never consults them.

## Linting your skills

`agentty skills` lists every discovered skill with spec-lint diagnostics and exits non-zero on warnings — drop it in CI to catch a broken or mis-named skill before it ships.

```bash
agentty skills   # list + validate; exit 1 on warnings
```

:::note
Skills are compatible with Claude Code's `.claude/skills/` format, so an existing skill library works in agentty unchanged.
:::
