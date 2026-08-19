---
title: Configuration
description: Environment variables and on-disk paths agentty reads.
nav_section: User Manual
nav_order: 50
slug: configuration
---

agentty is configured through flags, environment variables, and two on-disk paths. There is no sprawling config file to learn.

## Environment variables

| Variable | Effect |
|---|---|
| `ANTHROPIC_API_KEY` | Claude API key used when no -k flag is passed. Second-highest priority in credential resolution. |
| `CLAUDE_CODE_OAUTH_TOKEN` | OAuth token from the env (reuses Claude Code's token) — below API key but above on-disk creds. No refresh token. |
| `OPENAI_API_KEY` | Key for --provider openai, and the fallback key for every other OpenAI-compatible provider. |
| `GROQ_API_KEY / OPENROUTER_API_KEY / TOGETHER_API_KEY / CEREBRAS_API_KEY` | Provider-specific keys, checked before OPENAI_API_KEY for that provider. Ollama needs none. |
| `AGENTTY_SOCKS_PROXY` | Route all TCP through this SOCKS5 proxy host:port (set automatically by airgap mode). |
| `AGENTTY_API_HOST` | Override the API host (host[:port]) — dial a different upstream, keeping normal TLS chain verification (and any `AGENTTY_TLS_PINS` you set). |
| `AGENTTY_OAUTH_HOST` | Override the OAuth host (host[:port]). |
| `AGENTTY_INSECURE` | Set to 1 to skip TLS peer verification. Last-resort only — never ship it. |
| `AGENTTY_TLS_PINS` | Opt-in public-key pinning: comma-separated base64 SHA-256 SPKI hashes; the handshake fails closed if the leaf cert's key matches none. Off by default. Include a backup pin so cert rotation can't lock you out. |
| `AGENTTY_USE_KEYSTORE` | Set to 1 to store credentials in the OS secret store (Linux libsecret / macOS Keychain / Windows Credential Manager) in addition to the file. Falls back to the file when no backend is present. |
| `AGENTTY_ENCRYPT_PASSPHRASE` | Set to 1 to encrypt the credentials file at rest with a passphrase (prompted on the tty, echo off). Sealed with AES-256-GCM under an Argon2id (or scrypt) key. |
| `AGENTTY_PASSPHRASE` | Supply the at-rest encryption passphrase non-interactively (CI/scripts) instead of the tty prompt. |
| `AGENTTY_KDF` | Set to `scrypt` to force the portable scrypt KDF instead of the default Argon2id for at-rest encryption. |
| `AGENTTY_AIRGAP_SSH` | Extra flags injected into the ssh invocation for airgap (laptop side). |
| `AGENTTY_CLIPBOARD_CMD` | Shell command that writes image bytes to stdout — used for Ctrl+V image paste over SSH. |
| `AGENTTY_MCP_CONFIG` | Explicit path to an mcp.json, overriding the project/user lookup. |
| `AGENTTY_MCP_ALLOW_PROJECT` | Blanket-trust a project-local .agentty/mcp.json so its stdio servers connect (gated off by default). Alternative to per-file [content-hash approval](/docs/plugins#config-scope). |
| `AGENTTY_DOCS_DIR` | Folder of documents to index for the search_docs [retrieval](/docs/retrieval) tool. Auto-discovers `./docs` then `./.agentty/knowledge` when unset. Even with no docs, `search_docs` still searches your installed **skills** and **learned memory**. |
| `AGENTTY_EMBED_MODEL / AGENTTY_OLLAMA_HOST` | Embedding model (default `nomic-embed-text`) + Ollama host (`host:port`, default `127.0.0.1:11434`) for the dense half of the hybrid RAG pipeline. No Ollama → dense falls back to a local hash embedder and hybrid still runs. |
| `AGENTTY_RAG_SKILLS / AGENTTY_RAG_MEMORY` | Fold installed skills / learned memory into the search_docs corpus. **On by default**; set `=0` to disable. |
| `AGENTTY_RAG_MCP` | Fold connected MCP `resources/*` into the search_docs corpus. Off unless truthy **and** an MCP config is present. |
| `AGENTTY_RAG_DENSE_WEIGHT / AGENTTY_RAG_BM25_WEIGHT` | Fusion weights for the dense (embedding) vs lexical (BM25) lists. Default `1.0` each. Fusion is **convex (TM2C2)** — it preserves score magnitude and, per rag-cpp's benchmarks, beats RRF on NDCG. |
| `AGENTTY_RAG_CONTEXTUAL` | Anthropic **Contextual Retrieval**: situate each chunk in its document before indexing so a fragment that lost its heading still ranks. **On by default** (deterministic extractive fallback — no model, index-time cost only); `=0` disables. |
| `AGENTTY_RAG_PRF` | Pseudo-relevance-feedback (RM3-lite) query expansion: harvest the most discriminative terms from the top BM25 hits and fuse a second, down-weighted probe. **On by default** (deterministic, sub-ms); `=0` disables. |
| `AGENTTY_RAG_MMR / AGENTTY_RAG_MMR_LAMBDA` | MMR diversification: drop near-duplicate passages so the model sees distinct information. **On by default**; `_LAMBDA` trades relevance vs diversity (default `0.5`); `AGENTTY_RAG_MMR=0` disables. |
| `AGENTTY_RAG_STITCH` | Parent-document stitch: after ranking, fold each surviving chunk back into its adjacent siblings so the model reads the hit in context. **On by default** (in-memory, no network); `=0` disables. |
| `AGENTTY_RAG_GRAPH` | GraphRAG **local multi-hop expansion**: seed with hybrid, walk the document graph (links + shared-entity edges) and fuse in supporting passages a flat index misses. **On by default** (deterministic, in-memory); `=0` disables. |
| `AGENTTY_RAG_CORRECT` | **CRAG** corrective grading: a model-free retrieval evaluator drops passages graded irrelevant and yields a calibrated confidence in `[0,1]` — the signal the proactive-injection gate reads. **On by default**; `=0` disables. |
| `AGENTTY_RAG_OUTPUT_BYTES` | Aggregate body budget across all returned passages (default `12288` ≈ 3k tokens). Clamped 2 KiB – 64 KiB. |
| `AGENTTY_RAG_RELEVANCE_FLOOR` | **Token frugality:** drop tail passages scoring below `frac × top` before spending any tokens on them — the model ignores the low-confidence tail anyway. The best hit is always kept. Default `0.30`; `0` keeps every hit. |
| `AGENTTY_RAG_BUDGET_GAMMA` | **Token frugality:** water-filling budget split — allocate bytes ∝ `score^gamma` (with a per-passage floor) instead of an even total/n split, so confident passages get room to be complete and marginal ones get a tight excerpt. Default `1.5`; higher concentrates budget on the head. |
| `AGENTTY_RAG_CONF_BUDGET_FLOOR` | **Token frugality:** scale the *total* budget by CRAG confidence — a barely-passing retrieval injects a cheap block, a slam-dunk gets full room. Linear ramp from this floor fraction at low confidence to 100% at full confidence. Default `0.45`. |
| `AGENTTY_RAG_EXTRACTIVE` | **Token frugality:** model-free LLMLingua-style extractive compression for prose — keep the highest query-overlap sentences within budget and drop the filler *between* them (code/config keep a contiguous line-window). **On by default**; `=0` forces the line-window path. |
| `AGENTTY_RAG_PROACTIVE_BYTES` | **Token frugality:** independent, tighter cap on the *unprompted* proactive `<retrieved-context>` block (spent without the user asking). Default `6144` ≈ 1.5k tokens; clamped 1 KiB – 32 KiB. |
| `AGENTTY_RAG_MEASURE` | Set `=1` so `agentty rag-bench` also runs its queries through the real retrieval output path and reports bytes/estimated-tokens per query plus a machine-readable `MEASURE` line — diff two runs with a lever toggled to quantify its saving. Measurement only; **off by default**. |
| `AGENTTY_RAG_EXPAND / AGENTTY_RAG_HYDE / AGENTTY_RAG_GEN_MODEL` | LLM-assisted recall: **multi-query / RAG-Fusion** (`_EXPAND`) and **HyDE** (`_HYDE`), both opt-in (`=1`), powered by a small **local** Ollama model (`_GEN_MODEL`, default `qwen2.5:0.5b`) on the same Ollama used for embeddings. If Ollama is unreachable they silently no-op and plain hybrid still runs; they are also skipped on the latency-sensitive proactive pre-turn path. |
| `AGENTTY_RAG_PERSIST` | Cache the built docs index to `.agentty/rag_docs.ragdb` so a later session opens warm without re-walking + re-embedding. **On by default**; `=0` disables. |
| `AGENTTY_RAG_LEARN` | The **learning loop**: fold each passage's Beta-smoothed win-rate (`.agentty/rag_feedback.tsv`) back into ranking as a bounded (±15%) nudge, so passages that repeatedly prove useful **in this workspace** edge ahead of near-tied ones. A `read` of a path retrieval just surfaced counts as a win; unseen paths are untouched (neutral). **On by default**; `=0` disables (delete the TSV to forget). |
| `AGENTTY_RAG_TRACE` | Fold rag-cpp's per-stage trace into the retrieval `mode` string for debugging. **Off by default**; truthy enables. |
| `AGENTTY_RAG_PROACTIVE / AGENTTY_RAG_PROACTIVE_MIN` | Pre-turn auto-retrieval that injects a `<retrieved-context>` block when a query looks knowledge-shaped. On by default; `=0` disables. `_MIN` is the CRAG-calibrated confidence bar to inject (default `0.35`). |
| `AGENTTY_RAG_PROACTIVE_BUDGET_MS` | Fast-path **hedge** for the proactive pre-turn retrieval. The submit thread waits at most this long for the funnel; if it lands, grounding is injected inline with zero added latency. If it overruns the submit thread never blocks — the turn enters its normal streaming state (status shows *retrieving context…*, the UI never feels hung) and the stream launch is HELD behind a background retrieval that injects the block **same-turn** the moment it lands. Grounding is always for the question just asked; it is never dropped or deferred to a later turn. Default `250`; `0` skips the inline hedge. |
| `BM25_USE_STEMMER / BM25_HEADING_BOOST` | Lexical tuning. Porter stemming ("run/runs/running" match) is **on by default**; set `BM25_USE_STEMMER=0` to disable (e.g. a code-symbol corpus). `BM25_HEADING_BOOST` (default 3) is how many times a chunk's heading breadcrumb is folded into its BM25 tokens — heading matches out-score body matches; 1 disables the boost. |
| `AGENTTY_DEBUG_API / AGENTTY_DEBUG_FILE` | Set AGENTTY_DEBUG_API=1 to dump streaming provider events to AGENTTY_DEBUG_FILE. |
| `SSL_CERT_FILE / SSL_CERT_DIR / CURL_CA_BUNDLE` | Override the TLS root store agentty trusts (standard OpenSSL vars). |

### Smart Mode tuning

The Smart Mode *feature* toggles (which layers run) live in the `Ctrl+S` overlay and persist to `settings.json`. These are the numeric **policy** knobs below that — advanced tuning, read at point of use and clamped to a safe range (unset ⇒ the shipped default). Only genuine policy is exposed; the signature hash space, storage compaction thresholds, and individual classifier feature weights are deliberately fixed (changing them corrupts stored learning or breaks invariants).

| Variable | Meaning |
|----------|---------|
| `AGENTTY_SMART_COMPLEX_THRESHOLD` | Feature-score at/above which a turn classifies as **Complex** (more reasoning, more cost). Lower ⇒ more turns escalate; higher ⇒ fewer. The Simple/Standard boundary tracks it. Default `3`; range 1–8. |
| `AGENTTY_SMART_DEEP_MARGIN` | How far *into* a tier (score margin) a turn must sit to earn the extra **continuous effort** step — a genuinely hard Complex turn reaches +2 immediately instead of waiting for the session bias to drift. Lower ⇒ eager; higher ⇒ stays close to the discrete tier. Default `3`; range 1–8. |
| `AGENTTY_SMART_PRIOR_EVIDENCE` | Evidence pseudo-count before the per-workspace **learned routing prior** is trusted. Lower ⇒ the store reacts faster (fewer turns to move a prior); higher ⇒ more conservative. Default `5`; range 1–100. |
| `AGENTTY_SMART_BIAS_CLAMP` | Symmetric cap (±N steps) on the **session cascade bias** — how far this session's self-correction can drift effort from baseline. Default `2`; range 1–4. |

## On-disk paths

Credentials live under XDG config; everything else lives under `~/.agentty`.

- `~/.config/agentty/credentials.json` — Claude OAuth token or API key, mode `0600` (honours `$XDG_CONFIG_HOME`). Plaintext JSON by default; optionally sealed with AES-256-GCM (`AGENTTY_ENCRYPT_PASSPHRASE`) and/or stored in the OS keystore (`AGENTTY_USE_KEYSTORE`). See [Authentication](/docs/authentication) for the hardening options.
- `~/.agentty/settings.json` — persisted provider, model, per-provider models, reasoning effort, favourite models, permission profile, auto-compaction depth, and in-app-pasted provider keys.
- `~/.agentty/threads/<id>.json` — one JSON file per thread (flat, keyed by thread id).
- `~/.agentty/memory.jsonl` — user-scope `remember` facts (cross-workspace); `<project>/.agentty/memory.jsonl` holds project-scope facts. Which file a fact lands in is chosen by [memory scope](#memory-scope), below.
- `~/.agentty/skills/`, `~/.agents/skills/`, `~/.claude/skills/` — personal [Agent Skills](/docs/skills); the same three dirs under `<project>/` shadow them.
- `~/.agentty/mcp.json` (your servers, trusted) and `<project>/.agentty/mcp.json` (repo servers — command-spawning ones need [per-server approval](/docs/plugin-trust) or `AGENTTY_MCP_ALLOW_PROJECT`) — [MCP servers](/docs/mcp) to connect. `AGENTTY_MCP_CONFIG` overrides both.
- `~/.agentty/mcp_approvals.json` — content hashes of the project MCP servers you've approved (see [Plugin Trust](/docs/plugin-trust)); hooks use the sibling `hooks_approved.json`. A repo can't write to either, so it can't approve itself.
- `<project>/.agentty/rag_docs.ragdb` — the persisted [retrieval](/docs/retrieval) index (hybrid + dense vectors), so a later session opens warm without re-walking + re-embedding the docs folder. Rebuilt automatically when the corpus changes; delete to force a cold rebuild. Disable with `AGENTTY_RAG_PERSIST=0`.
- `<project>/.agentty/rag_feedback.tsv` — the [retrieval](/docs/retrieval) learning loop's per-passage use/win counts (human-inspectable TSV). Delete to forget.

## Memory scope

A `remember`ed fact lives in one of two files: **project** (`<project>/.agentty/memory.jsonl`, this codebase only) or **user** (`~/.agentty/memory.jsonl`, every workspace). Project is the default — most facts you keep are about the repo in front of you.

agentty routes a fact **smartly** so a personal preference never bleeds into every repo. When you (or the model) don't pin a scope, it reads the fact text: a fact plainly about *you* — first-person “I prefer” / “my name is”, personal tooling — is auto-routed to **user** scope, and the reply notes `scope→user (reason)`. A fact about the codebase — a build command, a source path, “in this project we…” — stays **project**.

The correction is **one-directional and conservative**: it only ever nudges project→user (a personal fact escaping into shared project memory is the harm; the reverse is merely narrow), only when the signal is confident, and an explicit scope always wins. It's a deterministic guardrail on the model's judgment, not a second model call.

Both files are plain JSONL you can inspect or hand-edit. Project scope is only offered when the project directory is writable — under `--workspace /` (no containing project) everything falls back to user scope. The same underlying scope-resolution logic also drives skills, agents, and slash-command discovery (project shadows user, native `.agentty` shadows the `.agents`/`.claude` interop dirs).

## CLAUDE.md guidance

On the Claude backend, agentty appends up to three user-authored guidance files to the system prompt (each capped at 64 KiB, mtime-cached):

- `~/CLAUDE.md` — user tier (every workspace).
- `<project>/CLAUDE.md` — project tier.
- `<project>/CLAUDE.local.md` — local tier (gitignore it for personal notes).

## AGENTS.md guidance

agentty also reads the [AGENTS.md](https://agents.md) open standard — stewarded by the [Agentic AI Foundation](https://aaif.io) under the Linux Foundation — for project-scoped agent guidance. Think of AGENTS.md as a README for agents: a dedicated, predictable place to provide build steps, test commands, and code-style conventions to AI coding agents. Over 60k open-source projects ship one.

agentty resolves AGENTS.md in three tiers (lowest precedence → highest):

### Global scope (~/.agentty/AGENTS.md)

Not part of the published spec (which is project-scoped only), but a pragmatic extension that major tools like Codex (`~/.codex/AGENTS.md`) and OpenCode (`~/.config/opencode/AGENTS.md`) implement. agentty checks two candidates in priority order — the first non-empty file wins:

1. `~/.agentty/AGENTS.md` — agentty-specific global guidance (highest priority).
2. `~/.agents/AGENTS.md` — shared global guidance for any agent tool (fallback).

When present, the content is injected as an `<agents-md-global>` block **before** the project-level block, so project guidance overrides global. Capped at 64 KiB.

### Project root (<project>/AGENTS.md)

Per the published spec, this is the primary file. agentty reads `<project>/AGENTS.md` (resolved from `--workspace`, not the raw process cwd). There is no local tier — that concern stays with CLAUDE.md. When present, the content is injected as an `<agents-md>` block in the system prompt, placed **before** the CLAUDE.md `<memory>` block, so standardized public project guidance lands first and personal/team memory layers on top. Capped at 64 KiB and mtime-cached (same pipeline as CLAUDE.md).

### Nested monorepo walk (nearest AGENTS.md)

The spec also describes nested files for monorepos: *"Place another AGENTS.md inside each package. Agents automatically read the nearest file in the directory tree, so the closest one takes precedence."* agentty implements this as an upward directory walk:

- When the agent's cwd (clamped inside the workspace via `project_root()`) is a subdirectory of `workspace_root`, agentty walks **upward** from the cwd toward the workspace root, looking for the closest `AGENTS.md` that is **not** the root file.
- The walk stops at the first file found (nearest wins) — it does not collect all files along the path. The nearest file's content is injected as an `<agents-md-package>` block, after the root block, so the model applies package-specific overrides where they conflict with root-level guidance.
- The walk never escapes the workspace boundary.
- If the nearest `AGENTS.md` is the same file as the root (same canonical path — e.g. the cwd is at the workspace root), no `<agents-md-package>` block is emitted (dedup).
- Capped at 64 KiB.

The walk starts from the agent's **cwd** (`project_root()`), not from the directory of a specific edited file — a TUI agent has a working directory, not a single "file being edited." In the common case (agent working inside a subpackage directory), the cwd and the relevant package directory coincide, so the nearest `AGENTS.md` is the package's own.

### Wire shape

When AGENTS.md files are present, agentty injects them as separate blocks in the system prompt (low → high precedence):

```
<agents-md-global>    ← ~/.agentty/AGENTS.md or ~/.agents/AGENTS.md (optional)
<agents-md>           ← <workspace_root>/AGENTS.md (project root)
<agents-md-package>   ← nearest nested AGENTS.md (monorepo walk, optional)
```

All three tiers are elided when their content is missing or empty, so adding AGENTS.md is purely additive — workspaces without one see no change. Each file is capped at 64 KiB.

### Coexistence with CLAUDE.md

AGENTS.md is the cross-tool public standard (works with Codex, Cursor, Jules, Aider, opencode, and many others — see the [full list](https://agents.md)), while CLAUDE.md remains agentty's personal/team memory hierarchy. A repo can ship AGENTS.md for cross-agent conventions and individual users can keep CLAUDE.md / CLAUDE.local.md for their own notes; both are injected, AGENTS.md first.

> **`AGENTS.md` is not the same as agentty's subagents.** Despite the similar name, `AGENTS.md` is a *document* — project guidance injected into the system prompt. It is unrelated to agentty's **subagents** (the delegate personas in `.agentty/agents/*.md` that the `task` tool spawns, shown in the command palette's *Subagents* entry). One is a project rulebook read into the prompt; the other defines *who* you can delegate to. AGENTS.md also does **not** apply to subagents — they run on a lean prompt that excludes both AGENTS.md and CLAUDE.md memory.

## Persisted settings

`--provider`, `-m`/`--model`, the reasoning effort tier, favourited models, your permission profile, and your compaction depth are written to `~/.agentty/settings.json` whenever you change them in-app — so the next launch resumes exactly where you left off. There is nothing to hand-edit; the picker (`^P` / `^/`) and `S-Tab` manage it. Compaction depth is set from the command palette's *Compaction depth* entry — see [Providers](/docs/providers#1m-context-models) for why you'd raise it on a 1M-context model.

## Choosing a workspace

By default the launch directory is the workspace. Override without `cd`:

```bash
agentty --workspace ~/code/other-project
agentty --workspace /          # opt out of the boundary entirely
```

## TLS trust store

agentty picks up the system trust store at startup. Behind a TLS-terminating corporate proxy, install the proxy's CA into the system store (`update-ca-certificates` / `update-ca-trust`). See [Corporate Proxies](/docs/proxies).
