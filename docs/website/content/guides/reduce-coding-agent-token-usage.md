---
title: "How to cut your coding agent's token usage with retrieval (RAG)"
description: "Coding agents waste tokens by sending your whole repo to the model. Learn how agentty's built-in retrieval sends only relevant code — cutting context by 80%+."
competitor: "reduce coding agent token usage"
verdict: "Instead of stuffing your whole repository into the prompt, agentty retrieves only the relevant chunks with hybrid BM25 + dense search — often cutting context tokens by 80% or more."
updated: "2026-08-19"
---

# How to cut your coding agent's token usage with retrieval (RAG)

The most expensive habit in AI coding tools is sending too much context. Many agents dump large portions of your repo into every prompt. agentty does the opposite.

## The problem: whole-repo context

If a tool sends 50k tokens of your codebase on every turn, you pay for 50k tokens on every turn — and the model's attention is diluted across irrelevant code.

## The fix: retrieve only what matters

agentty has a built-in retrieval engine:

- **Hybrid search** — BM25 (lexical) + dense embeddings, so it finds relevant code by both keyword and meaning.
- **Code-aware chunks** — it splits along function/class boundaries, not arbitrary line windows.
- **HNSW ANN + GraphRAG expansion** — fast nearest-neighbour lookup plus graph-based expansion to pull in related definitions.
- **A learning loop** — retrieval sharpens the more you use it on a repo.

The result: only the few relevant chunks are sent to the model, not the whole file tree.

## How much does it save?

On large repositories, retrieval commonly cuts context from tens of thousands of tokens down to a few thousand — an 80%+ reduction — which means lower cost, faster responses, and better answers.

## Try it

```
curl -fsSL https://agentty.org/install.sh | sh
```

Retrieval is on by default. Read the [retrieval docs](/docs/retrieval/) for tuning.
