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
| `AGENTTY_MCP_ALLOW_PROJECT` | Set truthy to trust a project-local .agentty/mcp.json (gated off by default). |
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
| `AGENTTY_RAG_EXPAND / AGENTTY_RAG_HYDE / AGENTTY_RAG_GEN_MODEL` | LLM-assisted recall: **multi-query / RAG-Fusion** (`_EXPAND`) and **HyDE** (`_HYDE`), both opt-in (`=1`), powered by a small **local** Ollama model (`_GEN_MODEL`, default `qwen2.5:0.5b`) on the same Ollama used for embeddings. If Ollama is unreachable they silently no-op and plain hybrid still runs; they are also skipped on the latency-sensitive proactive pre-turn path. |
| `AGENTTY_RAG_PERSIST` | Cache the built docs index to `.agentty/rag_docs.ragdb` so a later session opens warm without re-walking + re-embedding. **On by default**; `=0` disables. |
| `AGENTTY_RAG_LEARN` | The **learning loop**: fold each passage's Beta-smoothed win-rate (`.agentty/rag_feedback.tsv`) back into ranking as a bounded (±15%) nudge, so passages that repeatedly prove useful **in this workspace** edge ahead of near-tied ones. A `read` of a path retrieval just surfaced counts as a win; unseen paths are untouched (neutral). **On by default**; `=0` disables (delete the TSV to forget). |
| `AGENTTY_RAG_TRACE` | Fold rag-cpp's per-stage trace into the retrieval `mode` string for debugging. **Off by default**; truthy enables. |
| `AGENTTY_RAG_PROACTIVE / AGENTTY_RAG_PROACTIVE_MIN` | Pre-turn auto-retrieval that injects a `<retrieved-context>` block when a query looks knowledge-shaped. On by default; `=0` disables. `_MIN` is the CRAG-calibrated confidence bar to inject (default `0.35`). |
| `AGENTTY_RAG_PROACTIVE_BUDGET_MS` | Fast-path **hedge** for the proactive pre-turn retrieval. The submit thread waits at most this long for the funnel; if it lands, grounding is injected inline with zero added latency. If it overruns the submit thread never blocks — the turn enters its normal streaming state (status shows *retrieving context…*, the UI never feels hung) and the stream launch is HELD behind a background retrieval that injects the block **same-turn** the moment it lands. Grounding is always for the question just asked; it is never dropped or deferred to a later turn. Default `250`; `0` skips the inline hedge. |
| `BM25_USE_STEMMER / BM25_HEADING_BOOST` | Lexical tuning. Porter stemming ("run/runs/running" match) is **on by default**; set `BM25_USE_STEMMER=0` to disable (e.g. a code-symbol corpus). `BM25_HEADING_BOOST` (default 3) is how many times a chunk's heading breadcrumb is folded into its BM25 tokens — heading matches out-score body matches; 1 disables the boost. |
| `AGENTTY_DEBUG_API / AGENTTY_DEBUG_FILE` | Set AGENTTY_DEBUG_API=1 to dump streaming provider events to AGENTTY_DEBUG_FILE. |
| `SSL_CERT_FILE / SSL_CERT_DIR / CURL_CA_BUNDLE` | Override the TLS root store agentty trusts (standard OpenSSL vars). |

## On-disk paths

Credentials live under XDG config; everything else lives under `~/.agentty`.

- `~/.config/agentty/credentials.json` — Claude OAuth token or API key, mode `0600` (honours `$XDG_CONFIG_HOME`). Plaintext JSON by default; optionally sealed with AES-256-GCM (`AGENTTY_ENCRYPT_PASSPHRASE`) and/or stored in the OS keystore (`AGENTTY_USE_KEYSTORE`). See [Authentication](/docs/authentication) for the hardening options.
- `~/.agentty/settings.json` — persisted provider, model, per-provider models, reasoning effort, favourite models, permission profile, auto-compaction depth, and in-app-pasted provider keys.
- `~/.agentty/threads/<id>.json` — one JSON file per thread (flat, keyed by thread id).
- `~/.agentty/memory.jsonl` — user-scope `remember` facts (cross-workspace); `<project>/.agentty/memory.jsonl` holds project-scope facts.
- `~/.agentty/skills/`, `~/.agents/skills/`, `~/.claude/skills/` — personal [Agent Skills](/docs/skills); the same three dirs under `<project>/` shadow them.
- `~/.agentty/mcp.json` (trusted) and `<project>/.agentty/mcp.json` (gated behind `AGENTTY_MCP_ALLOW_PROJECT`) — [MCP servers](/docs/mcp) to connect on startup. `AGENTTY_MCP_CONFIG` overrides both.
- `<project>/.agentty/rag_docs.ragdb` — the persisted [retrieval](/docs/retrieval) index (hybrid + dense vectors), so a later session opens warm without re-walking + re-embedding the docs folder. Rebuilt automatically when the corpus changes; delete to force a cold rebuild. Disable with `AGENTTY_RAG_PERSIST=0`.
- `<project>/.agentty/rag_feedback.tsv` — the [retrieval](/docs/retrieval) learning loop's per-passage use/win counts (human-inspectable TSV). Delete to forget.

## CLAUDE.md guidance

On the Claude backend, agentty appends up to three user-authored guidance files to the system prompt (each capped at 64 KiB, mtime-cached):

- `~/CLAUDE.md` — user tier (every workspace).
- `<project>/CLAUDE.md` — project tier.
- `<project>/CLAUDE.local.md` — local tier (gitignore it for personal notes).

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
