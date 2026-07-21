---
title: CLI Reference
description: Every agentty subcommand and flag.
nav_section: User Manual
nav_order: 60
slug: cli
---

agentty is one binary with a handful of subcommands and flags.

## Running

```bash
agentty                                # run in the current directory
agentty -w ~/code/project              # run against another workspace
agentty -m claude-opus-4-5             # pick a model for the session
agentty --provider openai -m gpt-4o    # run against a different backend
agentty --provider ollama              # local model, no key, no cloud
agentty -k sk-ant-…                     # single-session key, never written to disk
agentty --sandbox on                   # require an OS sandbox for bash/diagnostics
```

## Subcommands

| Command | What it does |
|---|---|
| `agentty` | Start an interactive thread in the workspace. |
| `agentty login` | Authenticate non-interactively (useful over SSH). |
| `agentty logout` | Clear stored credentials. |
| `agentty status` | Print which auth source will be used. |
| `agentty airgap user@host` | Run the agent on a remote host through an SSH SOCKS tunnel. |
| `agentty acp` | Run headless as an [Agent Client Protocol](/docs/acp) agent for Zed (JSON-RPC over stdio). |
| `agentty mcp-serve` | Serve agentty's native tools over [MCP](/docs/mcp) (stdio). Point any MCP client at it. |
| `agentty skills` | List discovered [Agent Skills](/docs/skills) with spec-lint diagnostics (exit 1 on warnings — CI-friendly). |
| `agentty rag-bench [dir]` | Benchmark [`search_docs` retrieval](/docs/retrieval#measure-it-agentty-rag-bench) on your own corpus — recall@k / MRR / nDCG per pipeline stage. Defaults to the auto-discovered docs folder; pass a directory to override. |
| `agentty --version` | Print `agentty <version>` and exit. |
| `agentty --help` | Print usage and exit. |

## Command palette

Inside a thread there are no typed `/command` literals — press `^K` (or `/` on an empty composer) to open a fuzzy command palette. The fixed command set:

| Command | What it does |
|---|---|
| New thread | Start a fresh conversation. |
| Compact context | Replace history with a structured summary to reclaim the context window. |
| Review changes | Open the diff review pane (also `^R`). |
| Accept all / Reject all changes | Apply or discard every pending hunk. |
| Rewind to checkpoint | Jump to an earlier turn's worktree snapshot via a diff-preview picker (git repo + idle session; see [Checkpoints](/docs/threads#checkpoints)). |
| Cycle profile | Write → Ask → Minimal (also `S-Tab`). |
| Open model picker / Switch provider | Change the active model (`^/`) or backend (`^P`). |
| Open threads / Open plan | Browse saved conversations (`^J`) or view the todo plan (`^T`). |
| Run code block | Run a fenced block from the last reply (`^G`). |
| Login / Quit | Sign in, or exit agentty. |

## Options

These mirror `agentty --help` exactly.

| Flag | Effect |
|---|---|
| `-k`, `--key <key>` | API-key override for this session; never written to disk. |
| `-m`, `--model <id>` | Model id for the session (e.g. `claude-opus-4-5`). |
| `--provider <p>` | LLM backend: `anthropic` (default) or an OpenAI-compatible one — `openai` · `groq` · `openrouter` · `together` · `cerebras` · `ollama`, or a raw `host:port`. Persisted like `-m`; switch live with `^P`. See [Providers & Models](/docs/providers). |
| `-p`, `--profile <mode>` | ACP permission tier (Zed shows the prompts): `ask` (default) · `minimal` (also prompt reads) · `write` (never prompts — fully autonomous). |
| `-w`, `--workspace <dir>` | Sandbox filesystem tools to this directory (default: cwd). Tools refuse paths outside it. Pass `--workspace /` to disable the gate. |
| `--sandbox <mode>` | Wrap `bash`/`diagnostics` in an OS-native sandbox. `auto` (default) · `on` (require a backend) · `off` (disable). |
| `-V`, `--version` | Print the agentty version and exit. |
| `-h`, `--help` | Show usage and exit. |

## Air-gap flags

Passed to `agentty airgap` — see `agentty airgap --help`.

| Flag | Effect |
|---|---|
| `--setup` | Copy credentials to the remote on first run. |
| `--acp` | Print a ready-to-paste Zed `agent_servers` config that tunnels `agentty acp` over ssh stdio. Flags after it are forwarded to the remote agent. See [Zed / ACP](/docs/acp#airgap). |
| `--remote-agentty <path>` | Path to agentty on the remote if it isn't on `PATH`. |
