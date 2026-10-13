---
title: "agentty 0.2.12 — Zed-native ACP parity, a smarter retrieval pipeline, and a hardened composer"
date: 2026-08-04
author: agentty
tags: [release, acp, retrieval, editor]
excerpt: "0.2.12 closes the remaining ACP gaps with Zed's native agent (slash commands, model picker, sign-in, line-numbered reads), upgrades retrieval to rag-cpp's measured-best pipeline with a visible retrieval funnel and ~13% fewer tokens per query, and fixes six composer editing bugs including a queued-message data-loss bug."
---

# agentty 0.2.12

A dense release: **ACP shell parity** with Zed's own agent, a **retrieval
quality upgrade** with a funnel you can actually see, and a **composer
editing sweep** that closes six real bugs, one of them data loss. Full
detail in the [changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md).

## ACP: agentty now looks native inside Zed

Running agentty as an [ACP](/docs/acp) agent used to feel like a
second-class citizen next to Zed's own agent. Four gaps closed:

- **Slash-command menu.** Session start now emits `available_commands_update`,
  so Zed's composer `/` menu is populated with `/compact`, `/new`, and every
  installed skill as `/<skill-name>`.
- **Model picker.** A `model` `config_option_update` advertises the
  provider's full model catalog, so you can switch models from Zed's
  per-session dropdown instead of relaunching with `-m`.
- **Sign-in prompt.** When agentty has no credentials, `initialize`
  advertises an `authMethod`, so Zed renders a proper "Sign in" affordance
  instead of erroring on the first turn.
- **Live thread titles.** The first message of a session pushes a derived
  title via `session_info_update`, and restored sessions re-announce it on
  load, so Zed's thread sidebar always shows something meaningful instead of
  a blank or stale name.

Tool cards got the same treatment. A completed `read` now renders as a
fenced, tab-numbered excerpt (`1\talpha`, `2\tbeta`, …) matching
`claude-code-acp`'s gutter view. All non-diff tool output is markdown-escaped
so literal `*`, backticks, `#`, `|`, and `<` in file/grep/command output
render verbatim instead of being parsed as bold/headers/tables/HTML. Two
rendering bugs are fixed too: a `bash` run through the live-terminal fast
path used to show **nothing** once the terminal widget was released — the
completion now also carries the captured stdout/stderr as a fenced block —
and `write` now announces the same before/after diff shape as `edit`
instead of a distinct "new file" affordance. Every card that used to render
as a bare title now has a body, on both first run and session replay.

## Retrieval: rag-cpp's measured-best pipeline, and you can see why

`search_docs`/`search_code` upgrade to **adaptive convex (TM2C2) fusion**
in place of plain reciprocal-rank fusion — rag-cpp's benchmarks show it
beating RRF on NDCG because it preserves the score distribution RRF
discards, and the adaptive variant shifts weight toward whichever retriever
(lexical vs. dense) is more confident on a given query. Two new refinement
stages ride along: **near-duplicate dedup** (folds paraphrase/boilerplate
copies so the model isn't re-reading the same passage twice) and
**relevance autocut** (trims the low-relevance tail at the score knee, so a
query with three strong answers returns three, not a fixed *k* padded with
weak matches).

Retrieval used to be a black box — a ranked list and a terse
`(mode: hybrid+ctx, reranked)` label. Every result is now headed by a
readable **funnel** that walks the real per-stage counts the engine
recorded:

```
hybrid: 47 candidates ↳ reranked top 30 ↳ dedup 30→24 ↳ stitch: merged 3 adjacent ↳ autocut 24→8 ↳ top-8
```

And it's cheaper, not just clearer: five frugality levers on the output
path — a relevance floor, score-proportional water-filling of the token
budget, CRAG-confidence-scaled total budget, an LLMLingua-style extractive
compression pass for oversized prose passages, and a tighter cap on
unprompted proactive context — cut retrieval output **~13%** (1,845 → 1,605
estimated tokens/query on a 161-file corpus) with **identical** ranking
quality (recall@10 1.000, MRR 0.968, nDCG@10 0.976). Every lever is
on-by-default and tunable via env var; `AGENTTY_RAG_MEASURE=1 agentty
rag-bench` quantifies any one of them on your own corpus. See
[Retrieval](/docs/retrieval) for the full knob list.

## Composer: a data-loss bug and five papercuts, fixed

A sweep across the composer's reducer, view, and widget layers:

1. **Editing a peeked queued message could silently delete the wrong one.**
   After `Alt+↑` loaded a queued message for editing, the peek index
   survived an ordinary keystroke, so `Enter` sometimes removed a
   *different* queue slot than the one on screen. A stray edit now cleanly
   drops the peek and becomes the live draft.
2. **Undo now rewinds word-by-word, not character-by-character.** `Ctrl+Z`
   used to undo one character at a time, so a long sentence blew the whole
   64-deep undo history. Consecutive typing now coalesces into one undo
   unit, broken on whitespace and any non-typing op.
3. **The live token/word/line counters were wrong with an attachment
   present** — they counted the short chip caption instead of the payload,
   so a 400-line paste read as "1 line, ~10 tok." The counter now measures
   the expanded attachment body, i.e. what actually goes to the model.
4. **`Ctrl+←/→` word motion** now steps over a run of punctuation as one
   unit (`))))` used to take four presses).
5. **The `/` command palette** opens on any line-leading slash, not only a
   completely empty composer, matching shell muscle memory.

Kill-to-end-of-line is back too, rebound to **`Alt+K`** (`Ctrl+K` is claimed
by the command palette), pairing with `Ctrl+U` (kill-to-start) the same way
`Alt+D` pairs with `Ctrl+W`. The full composer keymap is now documented in
the README and the [keybindings](/docs/keybindings) page.

## Also in this line

- Thread list startup on a large history went from ~1000 ms to **&lt;1 ms**
  (~1400×) via a small `threads/index.json` metadata sidecar, self-healing
  if missing or corrupt.

## Get it

```bash
curl -fsSL https://agentty.org/install.sh | sh
# or
brew tap 1ay1/tap && brew install agentty
```

See the [full changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md)
for every detail.
