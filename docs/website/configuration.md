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
| `AGENTTY_API_HOST` | Override the API host (host[:port]) — dial a different upstream while keeping TLS pinning. |
| `AGENTTY_OAUTH_HOST` | Override the OAuth host (host[:port]). |
| `AGENTTY_INSECURE` | Set to 1 to skip TLS peer verification. Last-resort only — never ship it. |
| `AGENTTY_AIRGAP_SSH` | Extra flags injected into the ssh invocation for airgap (laptop side). |
| `AGENTTY_CLIPBOARD_CMD` | Shell command that writes image bytes to stdout — used for Ctrl+V image paste over SSH. |
| `AGENTTY_MCP_CONFIG` | Explicit path to an mcp.json, overriding the project/user lookup. |
| `AGENTTY_MCP_ALLOW_PROJECT` | Set truthy to trust a project-local .agentty/mcp.json (gated off by default). |
| `AGENTTY_DOCS_DIR` | Folder of documents to index for the search_docs RAG tool. Auto-discovers `./docs` then `./.agentty/knowledge` when unset. Even with no docs, `search_docs` still searches your installed **skills** and **learned memory**. |
| `AGENTTY_EMBED_MODEL / AGENTTY_OLLAMA_HOST` | Embedding model (default `nomic-embed-text`) + Ollama host (`host:port`) for the dense half of the hybrid RAG pipeline. No Ollama → RAG degrades to BM25 only, still works. |
| `AGENTTY_RAG_SKILLS / AGENTTY_RAG_MEMORY` | Fold installed skills / learned memory into the search_docs corpus. **On by default** (local, BM25-only, sub-ms); set `=0` to disable. |
| `AGENTTY_RAG_PROACTIVE / AGENTTY_RAG_PROACTIVE_MIN` | Pre-turn auto-retrieval that injects a `<retrieved-context>` block when a query looks knowledge-shaped. On by default; `=0` disables. `_MIN` is the confidence bar to inject (default `0.45`). |
| `AGENTTY_RAG_EMBED_RERANK / AGENTTY_RAG_RERANK_MODEL` | Batched embedding cross-encoder rerank (one `/api/embed` re-scores the candidate pool). On by default when embeddings are available; `=0` disables. `_MODEL` overrides the rerank embed model (else reuses the index model). |
| `AGENTTY_RAG_NEURAL / AGENTTY_RAG_NEURAL_MODEL` | Heavyweight generative cross-encoder rerank — one LLM call per candidate. **Off by default** (expensive); set truthy to enable. `_MODEL` picks the model (default `llama3.2`). |
| `AGENTTY_RAG_EXPAND / AGENTTY_RAG_EXPAND_MODEL / AGENTTY_RAG_EXPAND_N` | RAG-Fusion query expansion (LLM rewrites the query into N variants, fuses results). Off by default; truthy enables. `_N` is the variant count (default 4, 1–8). |
| `AGENTTY_RAG_HYDE` | HyDE (Hypothetical Document Embeddings): before retrieving, the LLM hallucinates a short answer-passage which is folded in as an extra RRF probe — lifts dense recall when the answer's vocabulary differs from the question's. **Off by default** (one generation per search); truthy enables. Reuses the expansion model. Composes with (and is independent of) `AGENTTY_RAG_EXPAND`. |
| `AGENTTY_RAG_CORRECT` | Corrective-RAG retry: on a low-confidence first pass, strip stopwords + widen the pool and retry. On by default; `=0` disables. |
| `AGENTTY_RAG_PARENT / AGENTTY_RAG_PARENT_RADIUS` | Parent-document (small-to-big) retrieval: after ranking, each surviving small chunk is stitched back into its adjacent sibling chunks from the same document so the model reads it in context. **On by default** (pure in-memory, no network); `=0` disables. `_RADIUS` is how many chunks to fold in on each side (default 1, 0–4). |
| `AGENTTY_RAG_MCP` | Fold connected MCP `resources/*` into the search_docs corpus. Off unless truthy **and** an MCP config is present. |
| `BM25_USE_STEMMER / BM25_HEADING_BOOST` | Lexical tuning. Porter stemming ("run/runs/running" match) is **on by default**; set `BM25_USE_STEMMER=0` to disable (e.g. a code-symbol corpus). `BM25_HEADING_BOOST` (default 3) is how many times a chunk's heading breadcrumb is folded into its BM25 tokens — heading matches out-score body matches; 1 disables the boost. |
| `AGENTTY_DEBUG_API / AGENTTY_DEBUG_FILE` | Set AGENTTY_DEBUG_API=1 to dump streaming provider events to AGENTTY_DEBUG_FILE. |
| `SSL_CERT_FILE / SSL_CERT_DIR / CURL_CA_BUNDLE` | Override the TLS root store agentty trusts (standard OpenSSL vars). |

## On-disk paths

Credentials live under XDG config; everything else lives under `~/.agentty`.

- `~/.config/agentty/credentials.json` — Claude OAuth token or API key, mode `0600` (honours `$XDG_CONFIG_HOME`).
- `~/.agentty/settings.json` — persisted provider, model, per-provider models, reasoning effort, favourite models, permission profile, and in-app-pasted provider keys.
- `~/.agentty/threads/<id>.json` — one JSON file per thread (flat, keyed by thread id).
- `~/.agentty/memory.jsonl` — user-scope `remember` facts (cross-workspace); `<project>/.agentty/memory.jsonl` holds project-scope facts.
- `~/.agentty/skills/`, `~/.agents/skills/`, `~/.claude/skills/` — personal [Agent Skills](/docs/skills); the same three dirs under `<project>/` shadow them.
- `~/.agentty/mcp.json` (trusted) and `<project>/.agentty/mcp.json` (gated behind `AGENTTY_MCP_ALLOW_PROJECT`) — [MCP servers](/docs/mcp) to connect on startup. `AGENTTY_MCP_CONFIG` overrides both.

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
