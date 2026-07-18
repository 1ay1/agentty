---
title: Retrieval (RAG)
description: How agentty's search_docs and search_code tools find the right passage — a fully local, dependency-free, offline-capable hybrid retrieval engine.
nav_section: Tools
nav_order: 40
slug: retrieval
---

agentty ships a complete, state-of-the-art **retrieval engine** behind two tools:

- **`search_docs`** — searches your *knowledge base*: a docs folder, your installed [skills](/docs/skills), your [learned memory](/docs/configuration#on-disk-paths), and (optionally) connected [MCP](/docs/mcp) resources.
- **`search_code`** — semantic search over your *source code* by meaning, for "where is retry backoff handled" style questions where you don't know the identifier.

The whole engine is **local, dependency-free, and degrades gracefully**. With no embedding model reachable it falls back to keyword (BM25) search and keeps working. Nothing is sent to the cloud; the only optional network hop is a *localhost* [Ollama](https://ollama.com) server for embeddings.

## The one-paragraph version

You ask a question. agentty retrieves a wide candidate pool with **hybrid search** (keyword BM25 + dense embeddings, fused), re-ranks it for precision, diversifies away near-duplicates, compresses each hit to its most relevant span, and stitches small chunks back into their surrounding context — then hands the model a short, high-signal, source-tagged set of passages. Every stage that costs nothing runs by default; anything that costs a model call is opt-in.

## What gets indexed

| Source | Default | Notes |
|---|---|---|
| **Docs folder** | Auto-discovered | `AGENTTY_DOCS_DIR`, else `./docs`, else `./.agentty/knowledge`. Incrementally cached on disk. |
| **Skills** | On | Your installed `SKILL.md` files. Disable with `AGENTTY_RAG_SKILLS=0`. |
| **Learned memory** | On | Facts you saved with `remember`. Disable with `AGENTTY_RAG_MEMORY=0`. |
| **MCP resources** | Off | A connected MCP server's `resources/*`. Enable with `AGENTTY_RAG_MCP=1`. |

Even with **no docs folder at all**, `search_docs` still searches your skills and memory — so it's useful from the first turn.

## The retrieval funnel

Every `search_docs` call runs this pipeline. The **default path uses no LLM calls** — it's fast and predictable.

### 1. Hybrid retrieval — cast a wide net

Two independent retrievers score every chunk, and their ranked lists are fused with **Reciprocal Rank Fusion (RRF)**:

- **BM25** (keyword) — catches exact terms, proper nouns, code symbols. Porter-stemmed by default so `run` / `runs` / `running` match.
- **Dense** (embeddings) — catches paraphrase and semantic near-matches. Uses a localhost Ollama embedding model (`nomic-embed-text` by default); at scale, an in-memory **HNSW** index makes the nearest-neighbour search sub-linear.

Dense is weighted slightly above lexical (`1.3×`) because semantic matching usually wins on natural-language docs — both weights are tunable (`AGENTTY_RAG_W_DENSE` / `AGENTTY_RAG_W_LEXICAL`).

### 2. Pseudo-relevance feedback — recover the words you didn't type *(default-on)*

A bare query rarely uses the exact vocabulary the docs use. agentty runs an initial BM25 pass, treats the top hits as *pseudo-relevant*, harvests their most **discriminative** terms (feedback frequency × rarity), and fuses in a second, down-weighted BM25 probe over `{query + those terms}`. A chunk that matches both the literal query *and* the expanded vocabulary is reinforced.

This is the classic **RM3** technique — deterministic, sub-millisecond, no model, no network — which is why it's **on by default**. Disable with `AGENTTY_RAG_PRF=0`; tune its weight with `AGENTTY_RAG_W_PRF`.

### 3. Contextual retrieval — a chunk knows where it lives *(default-on)*

Each chunk carries a *breadcrumb* of its document title and markdown heading path (`guide.md › Installation › Linux`). That breadcrumb is indexed on both the keyword and embedding sides, so a chunk that says "run the installer" is findable by "linux install" even though neither word appears in its body. (This is Anthropic's 2024 "contextual retrieval", built for free during chunking — no per-chunk model call.)

### 4. Re-ranking — precision at the top

First-pass fusion is recall-oriented and noisy at the very top, so the pool is re-scored:

- **Feature-fusion reranker** *(default-on)* — cheap, deterministic signals the first pass ignores: exact query-term coverage, phrase proximity, title/path match, and (when embeddings are present) calibrated cosine similarity. Pure C++, zero network.
- **Embedding cross-encoder rerank** *(default-on when embeddings are available)* — one batched `/api/embed` round-trip re-scores the candidate pool by fresh cosine. Degrades to the input order if the backend is unreachable, so it's safe to leave on.
- **Generative cross-encoder rerank** *(opt-in — `AGENTTY_RAG_NEURAL=1`)* — a graded 0–10 relevance judgment per candidate from a local generative model. The strongest reranker, but one LLM call per candidate, so it's off by default.

### 5. MMR diversification — no near-duplicates *(default-on)*

Maximal Marginal Relevance greedily keeps hits that are both relevant *and* different from what's already selected, so three overlapping windows of the same paragraph don't crowd out three distinct answers.

### 6. Compression — signal, not noise *(default-on)*

Rather than dump a whole 1600-char chunk into the model's window, each surviving hit is split into sentences, scored by query relevance, and trimmed to its best contiguous span. "20k noisy tokens" becomes "2k useful tokens."

### 7. Parent-document expansion — read it in context *(default-on)*

Small chunks retrieve *precisely* but read out of context. After ranking, each surviving chunk is stitched back into its **adjacent sibling chunks** from the same document, so the model sees the precise hit *inside* its surrounding prose — without widening the retrieval probe. Pure in-memory, no network. Tune the window with `AGENTTY_RAG_PARENT_RADIUS`; disable with `AGENTTY_RAG_PARENT=0`.

### 8. Corrective retry — a second chance on a weak result *(default-on)*

agentty computes a **confidence** signal for the result set. If the first pass looks weak, it strips stopwords, widens the pool, and retries — recovering hits when the original query was buried in conversational phrasing. Disable with `AGENTTY_RAG_CORRECT=0`.

## Opt-in recall boosters (they cost a model call)

Two of the strongest techniques need a generative model, so they add latency and are **off by default**. When enabled they help **every** knowledge configuration — docs, skills-only, memory-only, MCP, or any mix — because they feed extra probes into the same source-agnostic fusion.

- **RAG-Fusion query expansion** (`AGENTTY_RAG_EXPAND=1`) — the LLM rewrites your query into several alternative phrasings; each is retrieved and all results are fused. Vocabulary mismatch on any one phrasing stops being fatal. Variant count via `AGENTTY_RAG_EXPAND_N` (default 4).
- **HyDE — Hypothetical Document Embeddings** (`AGENTTY_RAG_HYDE=1`) — a question and its answer can sit far apart in embedding space. The LLM hallucinates a short *answer-passage* for your query; embedding that fake answer lands the probe near the real passages. The answer needn't be correct — only look like one. Composes with, and is independent of, query expansion.

## The proactive path

Beyond the explicit `search_docs` tool, agentty can retrieve **before you even ask**. When your message looks knowledge-shaped, it runs the funnel pre-turn and injects a `<retrieved-context>` block (source-tagged, deduplicated across turns) into the prompt — grounding the answer in your docs/skills/memory without a tool round-trip. This is on by default (`AGENTTY_RAG_PROACTIVE=1`) and only injects above a confidence bar (`AGENTTY_RAG_PROACTIVE_MIN`, default `0.45`).

## Provenance

Every returned passage is tagged with its source (`docs:`, `skill:`, `memory:`, or an MCP URI) and its file + line range. agentty never discards where a piece of information came from — cite it, open it, or follow it.

## Enabling the dense half

BM25 works with zero setup. To turn on the semantic half:

```bash
# 1. Run a local embedding model
ollama pull nomic-embed-text
ollama serve            # localhost:11434 by default

# 2. Point agentty at a docs folder (optional — skills/memory are always indexed)
export AGENTTY_DOCS_DIR=~/my-project/docs
```

That's it — agentty auto-detects the running Ollama server and upgrades from BM25-only to full hybrid retrieval. Override the model or host with `AGENTTY_EMBED_MODEL` / `AGENTTY_OLLAMA_HOST`.

## Full knob reference

Every environment variable — defaults, ranges, and effects — is documented in the [Configuration](/docs/configuration#environment-variables) table under the `AGENTTY_RAG_*` and `BM25_*` rows.

## Design notes

- **No dependencies.** BM25, RRF, HNSW, the reranker, MMR, compression, PRF, and the chunker are all in-house C++/STL. The only optional network hop is localhost Ollama.
- **Graceful degradation everywhere.** No embeddings → BM25-only. Ollama unreachable mid-search → the affected stage no-ops and retrieval continues. Empty corpus, blank query, zero-k → empty result, never an error.
- **Sensible defaults.** Everything that's free and safe runs by default; anything that costs a model call is opt-in, so the default `search_docs` is fast and predictable.
- **One seam for RAG and MCP.** From the model's view a docs folder and an MCP server are the same thing — a `KnowledgeSource` behind one interface — so they fuse identically.
