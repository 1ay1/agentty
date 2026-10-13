---
title: "agentty 0.2.11 — a cost-aware agent: cheaper compaction, cheaper subagents, real OAuth for MCP"
date: 2026-08-03
author: agentty
tags: [release, providers, mcp, performance]
excerpt: "The 0.2.10/0.2.11 line makes agentty economical by default — auto-compaction and read-only subagents now route to the cheapest capable model instead of the flagship you're chatting with, Claude's 1M-context variants are exposed correctly, MCP gets full interactive OAuth 2.1 + PKCE, and the model picker sorts by actual strength instead of a hardcoded family list."
---

# agentty 0.2.11 — a cost-aware agent

Two releases packed into one push: **v0.2.10** shipped a from-scratch-green
Windows build and per-user MSI install; **v0.2.11** is a session's worth of
work making agentty *economical* — compaction, subagent fan-out, and the
model catalog all got cheaper or more correct without changing how you use
the tool. Full detail, as always, in the [changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md).

## Compaction and subagents now cost less

Auto-compaction used to fire at a fixed absolute margin
(`context_max - 17k`) — inconsistent across window sizes (98% on a 1M
window, 91% on 200K) and it forced a summarization pass long before a
big-window model actually needed one. It now fires at a **percent of the
window** (default 90%, clamped to always leave 20K tokens of output
headroom), tunable from the command palette's new **Compaction depth**
entry (75% Aggressive → 90% Balanced → 95% Deep, persisted to
`settings.json`).

More important than the trigger: the summarization request itself now runs
on the **cheapest capable model** on your active provider instead of the
flagship model you're chatting with. Compacting used to mean a full
~context-max-sized flagship-priced input on every trigger; it now costs a
Haiku-class summary.

The same idea applies to `task` fan-out: read-only subagent roles
(`explorer`, `reviewer`) route to the cheapest model that still clears a
capability floor (tool support, dispatchable, non-Weak tier) — `tester`,
`coder`, and `general` keep the parent model since they mutate the
workspace. A single-model or Opus-only account sees no behavior change; a
multi-model account gets exploration for a fraction of the cost.

## 1M-context models, done Claude-Code-faithfully

Signed in with Claude Pro/Max OAuth, the model picker now offers a
**`(1M context)`** row right below every Sonnet/Opus/Haiku 4+ model —
verified against Claude Code's own shipped binary: the base window is
always 200K, and 1M is an explicit, entitlement-gated picker row, never
auto-detected from account tier. Picking it widens the tracked context
window and sends Anthropic's extended-context beta on your behalf; nothing
else changes about how you use the model. A raw API key doesn't get the row
(the beta is account-tier gated) — it's OAuth-only, matching reality.

## The picker sorts on strength, not a hand-written list

The old sort ordered models by a **hardcoded family bucket** — Opus,
Sonnet, Haiku, Fable, Mythos, in that fixed position — so Fable/Mythos
(Anthropic's newest flagship-tier lane, same strength class as Opus, just a
different codename) always sank to the bottom of the picker no matter how
new or capable it actually was. The new `agentty::model_picker_less`
comparator is the single source of truth for picker ordering: tier
descending (Flagship — Opus *and* Fable/Mythos — always leads), then newest
generation/revision within a tier, then a family tie-break for stable
grouping among peers. Any future family name sorts by what it *is*.

## MCP gets real interactive OAuth

`agentty mcp-login <server>` now walks an OAuth-gated MCP server end to
end: RFC 9728 protected-resource discovery, dynamic client registration (or
a CIMD `https://` client-id, or a pre-registered one — whichever the server
supports), a PKCE (S256) browser flow on an ephemeral loopback callback, and
a sealed, auto-refreshed token at `~/.agentty/mcp_tokens/<server>.json`
(chmod 600). `mcp-logout` clears it, `mcp-status` lists every configured
server and its authorization state. The whole path implements the
2026-07-28 MCP auth hardening: RFC 9207 issuer validation (closing the AS
mix-up attack), `application_type=native` DCR, issuer-bound credentials,
and CIMD support — all in the dependency-free, unit-tested `mcp-cpp` layer,
Windows-verified loopback server included.

## Provider ingress, fully unified

Every ingress concern shared across agentty's four transports — Anthropic,
OpenAI-compatible, Ollama-native, and ChatGPT/Codex's Responses API — now
lives in exactly one shared helper: Retry-After backoff parsing, strict
UTF-8 scrubbing, the leaked-tool-call sniffer, all three token-usage wire
shapes, and OpenAI-family auth headers. No more per-transport copies that
could silently drift; fixed two real bugs found while unifying (Ollama was
silently ignoring server backoff; two transports accepted invalid
UTF-8 the canonical scrubber now rejects).

## repo_map won't leak sibling projects

The ranked codebase skeleton `repo_map` returns could surface source from a
submodule, a vendored checkout, or an unrelated repo living under or
alongside your workspace. The walk now stops at any nested repository
boundary — including a submodule's gitlink *file*, not just a directory —
and re-asserts every accepted file is genuinely inside the workspace before
it becomes a graph node.

## Also in this line (v0.2.10)

- **Windows installs with no admin/UAC prompt.** The MSI is now `perUser`:
  installs to `%LocalAppData%\Programs\agentty`, edits only your `PATH`, no
  elevation required. The winget manifest matches (`Scope: user`).
- **Windows builds green end-to-end again.** Retrieval and the jetalloc
  allocator degrade gracefully on MSVC (no-op fallback) rather than
  blocking the whole release; the macOS build no longer trips configuring
  a GPU backend agentty's CPU-only retrieval never uses.
- **Package channels can't silently fall behind a release.** Every
  downstream publisher (AUR/Homebrew/scoop/winget) is gated behind its
  build leg, and a manifest-reconciliation job re-pins hashes from a
  release's `SHA256SUMS` automatically after every run and weekly.
- **Homebrew is a clean two-line install:** `brew tap 1ay1/tap && brew
  install agentty`.

## Get it

```bash
curl -fsSL https://agentty.org/install.sh | sh
# or
brew tap 1ay1/tap && brew install agentty
```

See the [full changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md)
and [Providers & Models](/docs/providers) for the 1M-context and picker
details.
