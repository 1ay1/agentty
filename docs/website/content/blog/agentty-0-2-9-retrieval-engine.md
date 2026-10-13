---
title: "agentty 0.2.9 — a frontier local retrieval engine, in one static binary"
date: 2026-07-27
author: agentty
tags: [release, retrieval, rag, performance]
excerpt: "The 0.2.x line took search_docs from a good local RAG to a frontier-grade one — hybrid BM25 + dense embeddings, HNSW ANN, GraphRAG expansion, a learning loop that closes the relevance feedback other agents leave open, and an eval harness that makes every stage provable. All pure C++/STL, offline-capable, zero new dependencies."
---

# agentty 0.2.9 — a frontier local retrieval engine, in one static binary

agentty has always shipped `search_docs`: local RAG over your docs, installed
skills, and learned memory. The 0.2.x line turned that from *good* into
**frontier-grade for a fully local, dependency-free, offline-capable engine** —
and, just as important, made the **default** path reflect it, not only the
fully-tuned one. Everything below is pure C++/STL. No new dependencies, no
service to run, and it degrades gracefully at every stage: no embeddings →
BM25-only, Ollama unreachable mid-search → the affected stage no-ops and
retrieval continues.

## The funnel

`search_docs` is not one algorithm — it's a ladder, and each rung earns its
place:

- **Hybrid BM25 + dense embeddings + RRF.** Lexical and semantic retrieval
  fused by Reciprocal Rank Fusion, so an exact-term hit and a
  meaning-only hit both survive.
- **HNSW ANN** for the dense side — the graph is built once and cached.
- **Contextual-retrieval breadcrumbs** — every chunk carries its file +
  heading path, so `guide.md › Install › Linux` is findable even when the body
  never says "linux".
- **PRF (RM3-lite) query expansion, default-on** — an initial BM25 pass harvests
  the most discriminative terms from the top hits and fuses a second,
  down-weighted probe, recovering vocabulary-mismatch hits (synonyms, the exact
  spelling the docs use) with zero model or network cost.
- **Parent-document (small-to-big) retrieval, default-on** — a precise small-chunk
  hit is stitched back into its adjacent siblings so it's read in context.
- **Feature-fusion + embedding cross-encoder rerank**, then **MMR**
  diversification and **extractive compression**.

New knobs like **HyDE** (hypothetical-document embeddings) are opt-in, and the
full engine now has a dedicated [Retrieval](/docs/retrieval) doc page that walks
the whole thing rung by rung.

## The engine now learns, converses, and hops

The headline of 0.2.9: retrieval that isn't static.

- **A learning loop.** agentty closes a feedback loop other terminal agents
  leave open. Every surfaced passage counts a "use"; when the agent follows up
  by `read`-ing the file that passage pointed at, that's an implicit "win." The
  Beta-smoothed per-passage win-rate persists to `.agentty/rag_feedback.tsv` and
  folds into ranking as a **bounded** multiplicative nudge (×0.85–×1.15, neutral
  with no history — a fresh workspace ranks byte-identically). Retrieval gets
  measurably better the more you use it.
- **Conversation carryover.** A recency-decayed salience pool lets a vague
  follow-up ("how does it handle errors?") pick up the entities already under
  discussion as an *extra* probe. It never replaces your query, so recall can
  only rise.
- **Multi-hop decomposition.** A compositional question ("how X works *and* how
  Y blocks Z") splits on clause connectives into per-facet probes, gated
  conservatively so ordinary queries pass untouched.
- **GraphRAG expansion.** The relevance graph hiding in your markdown links: the
  top hits' outbound `](doc.md)` targets are followed one hop and the linked
  documents join the context as supporting material, scored below every direct
  hit.

Each is default-on where deterministic and degrades to the previous behaviour on
any failure.

## Faster without getting weaker

A latency + throughput pass that changes *nothing* about default result quality:

- **One `/api/embed` round-trip per source, not N.** A fully-expanded search
  fans into many dense probes (query, carryover, RAG-Fusion paraphrases, a HyDE
  passage). They used to embed one at a time, serially — 5–8 blocking calls.
  They now batch into a single call per source.
- **Matryoshka ANN truncation** (opt-in) — walk the HNSW graph on a dimension
  *prefix* (e.g. 256 of 768) for ~2.3× faster walks at ⅓ the memory; the
  full-dimension rerank recovers precision.
- **Binary quantization** (opt-in) — 1-bit-per-dim sign codes with popcount
  Hamming for the walk, then exact float cosine on the returned pool: binary
  recall, float precision (~0.97 recall@5 in-repo), ~2.5× faster.

## `rag-bench` — every stage is provable

Retrieval engineering without measurement is vibes. `agentty rag-bench [dir]`
synthesizes known-item queries from your *own* corpus (deterministic, no LLM),
then reports recall@k / MRR / nDCG@10 / mean-µs across the ladder (BM25-only →
hybrid+PRF → +feature-rerank → +MMR). A regression or a win on any corpus is
attributable to exactly one stage — offline, in milliseconds.

## Also in 0.2.x

- **agentty identifies honestly to Anthropic** — no more spoofing the Claude
  Code CLI's client fingerprint to get subscription tokens accepted. It
  announces itself as `agentty/<version>` and keeps only what the API
  functionally requires.
- **`--auth-header NAME`** for OpenAI-compatible gateways that expect the key
  under a custom header (e.g. `X-API-Key`) instead of the standard bearer.
- **Rewind to any checkpoint, with a diff preview** — the command palette lists
  every checkpointed turn with an async `N files · +A −D` summary of what's
  changed since, so a rewind is never blind.
- **The Linux prebuilt is a *real* standalone binary** — `-static -no-pie`,
  guarded by a build-time ELF-shape assertion and a foreign-libc smoke test in
  clean Debian *and* Alpine containers before anything ships.

Full details are in the [changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md).
Grab the latest build:

```bash
curl -fsSL https://agentty.org/install.sh | sh
```
