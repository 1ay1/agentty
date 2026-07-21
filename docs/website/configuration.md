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
| `AGENTTY_EMBED_MODEL / AGENTTY_OLLAMA_HOST` | Embedding model (default `nomic-embed-text`) + Ollama host (`host:port`) for the dense half of the hybrid RAG pipeline. No Ollama → RAG degrades to BM25 only, still works. |
| `AGENTTY_RAG_SKILLS / AGENTTY_RAG_MEMORY` | Fold installed skills / learned memory into the search_docs corpus. **On by default** (local, BM25-only, sub-ms); set `=0` to disable. |
| `AGENTTY_RAG_PROACTIVE / AGENTTY_RAG_PROACTIVE_MIN` | Pre-turn auto-retrieval that injects a `<retrieved-context>` block when a query looks knowledge-shaped. On by default; `=0` disables. `_MIN` is the confidence bar to inject (default `0.45`). |
| `AGENTTY_RAG_PROACTIVE_BUDGET_MS` | Wall-clock budget for the proactive pre-turn retrieval, which runs on the submit thread. If the funnel (BM25 + dense query-embed + rerank/MMR over the whole index) can't finish within the budget it is abandoned this turn so the UI never freezes on Enter — the context folds in on a later turn from the now-warm cache. Default `350`; `0` never blocks the submit thread at all (retrieval always lands a turn late). Raise it to prefer same-turn grounding on a fast/small corpus. |
| `AGENTTY_RAG_EMBED_RERANK / AGENTTY_RAG_RERANK_MODEL` | Batched embedding cross-encoder rerank (one `/api/embed` re-scores the candidate pool). On by default when embeddings are available; `=0` disables. `_MODEL` overrides the rerank embed model (else reuses the index model). |
| `AGENTTY_RAG_NEURAL / AGENTTY_RAG_NEURAL_MODEL` | Heavyweight generative cross-encoder rerank — one LLM call per candidate. **Off by default** (expensive); set truthy to enable. `_MODEL` picks the model (default `llama3.2`). |
| `AGENTTY_RAG_EXPAND / AGENTTY_RAG_EXPAND_MODEL / AGENTTY_RAG_EXPAND_N` | RAG-Fusion query expansion (LLM rewrites the query into N variants, fuses results). Off by default; truthy enables. `_N` is the variant count (default 4, 1–8). |
| `AGENTTY_RAG_HYDE` | HyDE (Hypothetical Document Embeddings): before retrieving, the LLM hallucinates a short answer-passage which is folded in as an extra RRF probe — lifts dense recall when the answer's vocabulary differs from the question's. **Off by default** (one generation per search); truthy enables. Reuses the expansion model. Composes with (and is independent of) `AGENTTY_RAG_EXPAND`. |
| `AGENTTY_RAG_CORRECT` | Corrective-RAG retry: on a low-confidence first pass, strip stopwords + widen the pool and retry. On by default; `=0` disables. |
| `AGENTTY_RAG_PARENT / AGENTTY_RAG_PARENT_RADIUS` | Parent-document (small-to-big) retrieval: after ranking, each surviving small chunk is stitched back into its adjacent sibling chunks from the same document so the model reads it in context. **On by default** (pure in-memory, no network); `=0` disables. `_RADIUS` is how many chunks to fold in on each side (default 1, 0–4). |
| `AGENTTY_RAG_PRF / AGENTTY_RAG_W_PRF` | Pseudo-relevance-feedback (RM3-lite) query expansion: harvest the most discriminative terms from the top BM25 hits and fuse in a second, down-weighted BM25 probe over {query + those terms} — recovers vocabulary-mismatch hits (synonyms, the exact spelling the docs use) with **zero model/network cost**. **On by default** (deterministic, sub-ms); `AGENTTY_RAG_PRF=0` disables. `_W_PRF` sets the fusion weight of the expanded probe (default 0.5× the primary lexical list). |
| `AGENTTY_RAG_MCP` | Fold connected MCP `resources/*` into the search_docs corpus. Off unless truthy **and** an MCP config is present. |
| `AGENTTY_RAG_LEARN` | The learning loop: surfaced passages count a "use"; the agent `read`ing a surfaced file counts a "win", and the Beta-smoothed win-rate becomes a bounded per-passage rank nudge (×0.85–×1.15, neutral with no history) persisted to `.agentty/rag_feedback.tsv` (override with `AGENTTY_RAG_FEEDBACK_PATH`). **On by default**; `=0` disables. Delete the TSV to forget. |
| `AGENTTY_RAG_CARRYOVER` | Conversation carryover: a vague follow-up query ("how does **it** handle errors") gains the salient content terms of recent queries as an extra RRF probe. **On by default** (deterministic, no model); `=0` disables. |
| `AGENTTY_RAG_MULTIHOP` | Multi-hop decomposition: compositional queries split on clause connectives (" and ", " vs ", ";") into per-facet probes fused by RRF. Fires only when ≥2 clauses each carry ≥2 content terms. **On by default**; `=0` disables. |
| `AGENTTY_RAG_LATE` | Late-interaction (sentence MaxSim) rerank: re-score the top pool by each candidate's best sentence against the query — one batched `/api/embed` call, same cost as the chunk-level rerank it upgrades. **On by default when embeddings are available**; `=0` falls back to whole-chunk cosine. |
| `AGENTTY_RAG_GRAPH` | GraphRAG expansion: a memo-cached document graph (nodes = docs, edges = markdown links + tf·idf entity co-occurrence) supplies supporting chunks from four tiers around the top hits — outbound links, backlinks, entity neighbours, and the top hits' community hub (PageRank + deterministic label propagation). Always ranked below direct hits. **On by default** (deterministic, in-memory); `=0` disables. |
| `AGENTTY_RAG_GRAPH_SUMMARY / AGENTTY_RAG_GRAPH_SUMMARY_MODEL` | Community reports (full GraphRAG): the community hub passage carries a 2-3 sentence LLM overview of its whole cluster, generated **once per community per corpus shape** via local Ollama and persisted to `.agentty/rag_graph_summaries.tsv` (override with `AGENTTY_RAG_GRAPH_SUMMARIES_PATH`). **Off by default** (needs a generative model); truthy enables. `_MODEL` picks the model (default `llama3.2`). Failure degrades to the plain hub chunk. |
| `BM25_USE_STEMMER / BM25_HEADING_BOOST` | Lexical tuning. Porter stemming ("run/runs/running" match) is **on by default**; set `BM25_USE_STEMMER=0` to disable (e.g. a code-symbol corpus). `BM25_HEADING_BOOST` (default 3) is how many times a chunk's heading breadcrumb is folded into its BM25 tokens — heading matches out-score body matches; 1 disables the boost. |
| `AGENTTY_DEBUG_API / AGENTTY_DEBUG_FILE` | Set AGENTTY_DEBUG_API=1 to dump streaming provider events to AGENTTY_DEBUG_FILE. |
| `SSL_CERT_FILE / SSL_CERT_DIR / CURL_CA_BUNDLE` | Override the TLS root store agentty trusts (standard OpenSSL vars). |

## On-disk paths

Credentials live under XDG config; everything else lives under `~/.agentty`.

- `~/.config/agentty/credentials.json` — Claude OAuth token or API key, mode `0600` (honours `$XDG_CONFIG_HOME`). Plaintext JSON by default; optionally sealed with AES-256-GCM (`AGENTTY_ENCRYPT_PASSPHRASE`) and/or stored in the OS keystore (`AGENTTY_USE_KEYSTORE`). See [Authentication](/docs/auth) for the hardening options.
- `~/.agentty/settings.json` — persisted provider, model, per-provider models, reasoning effort, favourite models, permission profile, and in-app-pasted provider keys.
- `~/.agentty/threads/<id>.json` — one JSON file per thread (flat, keyed by thread id).
- `~/.agentty/memory.jsonl` — user-scope `remember` facts (cross-workspace); `<project>/.agentty/memory.jsonl` holds project-scope facts.
- `~/.agentty/skills/`, `~/.agents/skills/`, `~/.claude/skills/` — personal [Agent Skills](/docs/skills); the same three dirs under `<project>/` shadow them.
- `~/.agentty/mcp.json` (trusted) and `<project>/.agentty/mcp.json` (gated behind `AGENTTY_MCP_ALLOW_PROJECT`) — [MCP servers](/docs/mcp) to connect on startup. `AGENTTY_MCP_CONFIG` overrides both.
- `<project>/.agentty/rag_feedback.tsv` — the [retrieval](/docs/retrieval) learning loop's per-passage use/win counts (human-inspectable TSV). Delete to forget; override with `AGENTTY_RAG_FEEDBACK_PATH`, disable with `AGENTTY_RAG_LEARN=0`.
- `<project>/.agentty/rag_graph_summaries.tsv` — cached GraphRAG community reports, one per community per corpus shape (only written when `AGENTTY_RAG_GRAPH_SUMMARY=1`). Override with `AGENTTY_RAG_GRAPH_SUMMARIES_PATH`; delete to regenerate.

## CLAUDE.md guidance

On the Claude backend, agentty appends up to three user-authored guidance files to the system prompt (each capped at 64 KiB, mtime-cached):

- `~/CLAUDE.md` — user tier (every workspace).
- `<project>/CLAUDE.md` — project tier.
- `<project>/CLAUDE.local.md` — local tier (gitignore it for personal notes).

## Persisted settings

`--provider`, `-m`/`--model`, the reasoning effort tier, favourited models, and your permission profile are written to `~/.agentty/settings.json` whenever you change them in-app — so the next launch resumes exactly where you left off. There is nothing to hand-edit; the picker (`^P` / `^/`) and `S-Tab` manage it.

## Choosing a workspace

By default the launch directory is the workspace. Override without `cd`:

```bash
agentty --workspace ~/code/other-project
agentty --workspace /          # opt out of the boundary entirely
```

## TLS trust store

agentty picks up the system trust store at startup. Behind a TLS-terminating corporate proxy, install the proxy's CA into the system store (`update-ca-certificates` / `update-ca-trust`). See [Corporate Proxies](/docs/proxies).
