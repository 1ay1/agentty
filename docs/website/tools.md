---
title: Tool Overview
description: The full set of tools agentty can call, and how they render.
nav_section: Tools
nav_order: 10
slug: tools
---

Each tool gets a purpose-built widget: diffs render as diffs, search results group by file with line numbers, bash shows exit codes, todos become checklists.

| Tool | Effect class | Description |
|---|---|---|
| `read` | Read | Read a file (or a line range). Large files return a symbol outline first. |
| `write` | Write | Create a new file with atomic write semantics. |
| `edit` | Write | Apply targeted text substitutions to an existing file; renders a diff. |
| `move` | Write | Move or rename a file/directory without a shell. |
| `remove` | Write | Delete a file or directory (recursive requires an explicit flag). |
| `bash` | Shell | Run a shell command inside the sandbox; shows exit code + output. |
| `process_start` / `process_poll` / `process_stop` | Shell | Start, poll, and stop a long-running background process (dev servers, watchers) without blocking the turn. |
| `grep` | Read | Regex search across files, grouped by file with line numbers. |
| `glob` | Read | Find files by glob pattern. |
| `list_dir` | Read | List a directory with type, size, and name. |
| `repo_map` | Read | Token-budgeted, PageRank-ranked skeleton of the codebase — top files with definition signatures, personalizable with `focus`. The walk stops at any nested repo/submodule boundary and never leaves the workspace, so sibling projects can't leak into the map. THE tool to call first in a large or unfamiliar repo. |
| `find_definition` | Read | Locate a symbol definition across the codebase. |
| `find_references` | Read | Find exact identifier references, with enclosing symbol and context. |
| `web_fetch` | Network | Fetch a URL (capped output) for docs and APIs. |
| `web_search` | Network | Search the web and return result snippets. |
| `todo` | Pure | Maintain a session todo / plan list, rendered as a checklist. |
| `diagnostics` | Shell | Run the project's build/lint and surface errors and warnings. |
| `test` | Shell | Run focused project tests (CTest/Cargo/Go/npm/Make auto-detected) with structured pass/fail output. |
| `skill` | Pure | Load a named skill's full instructions from .agentty/skills/ before attempting a task it covers. |
| `task` | Network | Spawn an autonomous subagent (explorer / reviewer / tester / coder / general) with its own context and tool budget; returns one condensed report. Read-only roles (explorer, reviewer) automatically route to the cheapest capable model on the active provider — tester/coder/general keep the parent model — so fan-out exploration costs a fraction of the main turn. |
| `search_docs` | Network | Query your knowledge base — docs, installed skills, and learned memory — with agentty's hybrid BM25 + dense [retrieval engine](/docs/retrieval), reranked, diversified, and expanded over the corpus's [GraphRAG](/docs/retrieval#8-graphrag-expansion-retrieval-over-the-document-graph-default-on) document graph; returns the most relevant passages, source-tagged. Works with zero docs configured (skills + memory are always indexed). |
| `search_code` | Read | Semantic search over source code by *meaning*, not literal text — finds the relevant function for a conceptual query ("where is retry backoff handled") even with zero shared keywords. See [Retrieval](/docs/retrieval). |
| `git_status` | Read | Show branch, staged/unstaged changes, untracked files. |
| `git_diff` | Read | Show a diff (unstaged, staged, or a ref range). |
| `git_log` | Read | Show commit history. |
| `git_show` | Read | Show a commit's metadata + patch, or a file's contents at a revision. |
| `git_blame` | Read | Annotate a file or line range with the commit/author/date that last changed it. |
| `git_commit` | Write | Stage files and create a commit. |
| `remember / forget` | Pure | Persist or remove durable facts across sessions. |
| `wipe_memory` | Pure | Clear every remembered fact in a scope (confirm-gated). |

:::note
The **effect class** determines which permission profile auto-runs the tool. *Pure* and *Read* tools run automatically in **Ask** and **Write**; *Write*, *Shell*, and *Network* are gated by [your profile](/docs/profiles). The **Minimal** profile prompts on *every* class, reads included.
:::

## Compile-time enforcement

Each tool's effect set is declared at compile time and checked against the permission matrix via `static_assert`. A tool can't accidentally gain a side effect that the policy doesn't account for — the build catches it.
