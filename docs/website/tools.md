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
| `bash` | Shell | Run a shell command inside the sandbox; shows exit code + output. |
| `grep` | Read | Regex search across files, grouped by file with line numbers. |
| `glob` | Read | Find files by glob pattern. |
| `list_dir` | Read | List a directory with type, size, and name. |
| `find_definition` | Read | Locate a symbol definition across the codebase. |
| `web_fetch` | Network | Fetch a URL (capped output) for docs and APIs. |
| `web_search` | Network | Search the web and return result snippets. |
| `todo` | Pure | Maintain a session todo / plan list, rendered as a checklist. |
| `diagnostics` | Shell | Run the project's build/lint and surface errors and warnings. |
| `skill` | Pure | Load a named skill's full instructions from .agentty/skills/ before attempting a task it covers. |
| `task` | Network | Spawn an autonomous subagent (explorer / reviewer / tester / coder / general) with its own context and tool budget; returns one condensed report. |
| `search_docs` | Network | Query your knowledge base — docs, installed skills, and learned memory — with agentty's hybrid BM25 + dense RAG pipeline; returns the most relevant passages, source-tagged. Works with zero docs configured (skills + memory are always indexed). |
| `git_status` | Read | Show branch, staged/unstaged changes, untracked files. |
| `git_diff` | Read | Show a diff (unstaged, staged, or a ref range). |
| `git_log` | Read | Show commit history. |
| `git_commit` | Write | Stage files and create a commit. |
| `remember / forget` | Pure | Persist or remove durable facts across sessions. |
| `wipe_memory` | Pure | Clear every remembered fact in a scope (confirm-gated). |

:::note
The **effect class** determines which permission profile auto-runs the tool. *Pure* and *Read* tools run automatically in **Ask** and **Write**; *Write*, *Shell*, and *Network* are gated by [your profile](/docs/profiles). The **Minimal** profile prompts on *every* class, reads included.
:::

## Compile-time enforcement

Each tool's effect set is declared at compile time and checked against the permission matrix via `static_assert`. A tool can't accidentally gain a side effect that the policy doesn't account for — the build catches it.
