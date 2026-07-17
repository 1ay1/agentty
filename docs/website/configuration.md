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
| `AGENTTY_DOCS_DIR` | Folder of documents to index for the search_docs RAG tool (defaults to ./docs). |
| `AGENTTY_EMBED_MODEL / AGENTTY_OLLAMA_HOST` | Embedding model + Ollama host for the local search_docs RAG pipeline. |
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
