---
title: Storage
description: Every file agentty writes, where it lives, how long it stays, and how to move it.
nav_section: User Manual
nav_order: 52
slug: storage
---

Everything agentty writes, where it goes, how long it stays, and how to move it.

`agentty config` prints this for your machine, with real paths and sizes.
`agentty config env` lists every override and which ones are set.

## Two roots

agentty keeps its files in two places.

| root | follows | default | move it with |
|---|---|---|---|
| user root | you | `~/.agentty` | `$AGENTTY_HOME` |
| project root | the code | `<project>/.agentty` | `$AGENTTY_PROJECT_DIR` or `dirs.project` |

The **project** is the nearest folder above where you started agentty that
has a `.git`, `.agentty`, `.hg` or `.svn` in it. So `cd src && agentty` uses
the same project root as running it at the top of the repo. If there is no
marker, the start folder is the project. `/` is never one.

When `$AGENTTY_PROJECT_DIR` is set, every project gets its own subfolder
under it, named `<folder>-<hash>`, so two checkouts never share an index.

## What lives where

### User root, `~/.agentty` (0700)

| path | what | grows? | kept |
|---|---|---|---|
| `settings.json` | your settings | no | until you change them |
| `settings.json.lock` | lock so two instances don't clobber each other | no | always |
| `credentials/` (0700) | sign-ins and API keys: `credentials.json` (Claude), `accounts.json`, `provider-keys.json`, `embed_keys.json`, copilot/codex/kimi files, each 0600 | no | until you sign out |
| `threads/` (0700) | conversations: `<id>.jsonl` (the log), `<id>.ofs` (offsets), `<id>.meta.json`, `index.json` (picker cache), `<id>.transcript.md` (exports) | **yes** | until deleted, or `threads.keep_days` |
| `threads/blobs/` | images and big tool outputs, shared between threads by content hash | **yes** | swept daily once no thread uses them (24 h grace) |
| `memory.jsonl` | facts saved with `remember`, user scope | slowly | until forgotten |
| `state/` | approval hashes: `skills_approved.json`, `hooks_approved.json`, `mcp_approvals.json` | no | until revoked |
| `cache/` | `modelsdev.json` (model catalog), `update_check.json`, `copilot_model_support.json` | no | refetchable, safe to delete |
| `logs/` | `agentty.log`, `agentty.log.old`, `agentty-diagnostics.txt`, `agentty-flight.txt` | capped | log rotates at 32 MB, one old copy kept |
| `mcp.json`, `hooks.json` | your MCP servers and hooks | no | hand-edited |
| `skills/`, `agents/`, `commands/`, `AGENTS.md` | your skills, subagents, slash commands, global guidance | no | hand-edited |

### Project root, `<project>/.agentty`

It comes with a `.gitignore` of `*`, so none of it gets committed by
accident. Delete a line from it to start tracking something.

| path | what | kept |
|---|---|---|
| `cache/rag_code.<tag>.ragdb`, `cache/rag_docs.<tag>.ragdb` (+ `.meta.json`) | search indexes. `<tag>` names the embedder, so switching one never reads vectors from another | rebuildable. the newest 1 old variant is kept so switching back is cheap |
| `state/rag_feedback.tsv` | which search results helped, so ranking learns | until deleted |
| `memory.jsonl` | facts saved with `remember`, project scope | until forgotten |
| `mcp.json`, `skills/`, `agents/`, `commands/` | project config | committed by you if you want |

A project `mcp.json` starts processes, so it only runs after you approve its
exact bytes. The approval lives in the user `state/`, so a repo can never
approve itself.

### Read but never written

- `.agents/` and `.claude/` (project and user): skills, agents and commands
  in the shared markdown format. Same files, no conversion.
- `~/.claude.json`: only through `agentty mcp import --from claude`.

### Outside both roots

- hook payloads: `$TMPDIR/agentty_hook_<pid>_*`, 0600, removed after the hook runs.

## Moving things

Every directory can move on its own. Use the variable, or put the same thing
in `settings.json`:

```json
{
  "dirs": {
    "threads": "/mnt/big/agentty/threads",
    "logs": "~/.local/state/agentty"
  }
}
```

| what | variable | settings key |
|---|---|---|
| whole user root | `AGENTTY_HOME` | (variable only, it's where settings.json lives) |
| threads | `AGENTTY_THREADS_DIR` | `dirs.threads` |
| credentials | `AGENTTY_CREDENTIALS_DIR` | `dirs.credentials` |
| approvals | `AGENTTY_STATE_DIR` | `dirs.state` |
| cache | `AGENTTY_CACHE_DIR` | `dirs.cache` |
| logs | `AGENTTY_LOGS_DIR` | `dirs.logs` |
| whole project root | `AGENTTY_PROJECT_DIR` | `dirs.project` |
| search indexes | `AGENTTY_RAG_DIR` | `dirs.rag` |
| search feedback | `AGENTTY_PROJECT_STATE_DIR` | `dirs.project_state` |
| one MCP config file | `AGENTTY_MCP_CONFIG` | (a file, not a dir) |

Rules, the same for all of them:

- a set variable beats the settings key
- empty means unset
- `~/` expands in settings (your shell does it for variables)
- a relative path is relative to its root, never to your current folder.
  `AGENTTY_LOGS_DIR=logs2` is always `~/.agentty/logs2`
- if the folder can't be made, you get one warning naming why, and the
  default is used
- `credentials` and `threads` are forced to 0700 wherever they go

Nothing is moved for you. To move existing data, quit agentty, move the
folder, then set the override.

## Keeping it small

Threads are the only thing that grows without a limit. To expire them:

```json
{ "threads": { "keep_days": 90 } }
```

Once a day, threads with no activity for that many days are deleted, along
with their files. A thread open in a running agentty is never touched. Their
blobs go in the same pass if nothing else uses them. `0` or unset keeps
everything, which is the default.

Other things that clean themselves:

- blobs: once a day, anything no thread points at and older than 24 h
- search indexes: old embedder variants beyond the newest one, after a save
- logs: rotate at 32 MB

Files from older versions that nothing reads now (an old `~/.cache/agentty`,
`*.thread.ragdb`, `routing_memory.tsv`, `settings.json~`) are listed by
`agentty config clean` and deleted by `agentty config clean --yes`.

## Backing up

Copy `~/.agentty`. That's everything except per-project indexes, which
rebuild on their own. If you moved a directory out, copy that too;
`agentty config` shows where each one is.

Leave out `credentials/` if the backup goes somewhere less private than
your home folder.

## For contributors

One rule: **every path is declared once**, in
`include/agentty/config/inventory.hpp`, and nothing else builds one. A
`dirs::Spec` names the root, the leaf, the override variable and the
retention. `config_inventory_test` fails the build if a file declares its
own. `agentty config` prints the same table the code reads, so the two
can't disagree.

- user dirs resolve through `include/agentty/util/user_root.hpp`
- project dirs through `agentty::dirs::resolve` (`include/agentty/dirs/dirs.hpp`)
- settings `dirs` keys map to variables in `src/util/storage_env.cpp`, applied
  at the top of `main()` before any thread starts
- new storage: add a Spec to the inventory, a key to `storage_env.cpp`, and a
  row to the tables above

Design notes and history: `docs/design/data-dirs.md`, `docs/design/dot-agentty.md`.
