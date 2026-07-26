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

You ask a question. agentty retrieves a wide candidate pool with **hybrid search** (keyword BM25 + dense embeddings, fused), re-ranks it for precision, diversifies away near-duplicates, stitches small chunks back into their surrounding context, expands along the corpus's **document graph** (author-drawn links + inferred entity relationships) to pull in the connecting context a flat index misses, and grades the result set with a corrective evaluator that produces a calibrated confidence — then hands the model a short, high-signal, source-tagged set of passages. Every stage runs by default; the query-rewriting boosters (HyDE, multi-query) are powered by a small **local** Ollama model, so even they cost nothing beyond the localhost round-trip and silently no-op when Ollama is absent.

## What gets indexed

| Source | Default | Notes |
|---|---|---|
| **Docs folder** | Auto-discovered | `AGENTTY_DOCS_DIR`, else `./docs`, else `./.agentty/knowledge`. Incrementally cached on disk. |
| **Skills** | On | Your installed `SKILL.md` files. Disable with `AGENTTY_RAG_SKILLS=0`. |
| **Learned memory** | On | Facts you saved with `remember`. Disable with `AGENTTY_RAG_MEMORY=0`. |
| **MCP resources** | Off | A connected MCP server's `resources/*`. Enable with `AGENTTY_RAG_MCP=1`. |

Even with **no docs folder at all**, `search_docs` still searches your skills and memory — so it's useful from the first turn.

## The retrieval funnel

Every `search_docs` call runs this pipeline. The **default path uses no cloud calls** — it's fast, predictable, and fully local.

### 1. Hybrid retrieval — cast a wide net

Two independent retrievers score every chunk, and their ranked lists are fused with **convex (TM2C2) fusion**:

- **BM25** (keyword) — catches exact terms, proper nouns, code symbols. Porter-stemmed by default so `run` / `runs` / `running` match.
- **Dense** (embeddings) — catches paraphrase and semantic near-matches. Uses a localhost Ollama embedding model (`nomic-embed-text` by default); at scale, an in-memory **HNSW** index makes the nearest-neighbour search sub-linear.

Both retrievers carry equal weight by default (`1.0×` each), and both are tunable via `AGENTTY_RAG_BM25_WEIGHT` / `AGENTTY_RAG_DENSE_WEIGHT`.

**Fusion strategy.** The default is **convex (TM2C2) fusion** — it normalizes each retriever's scores and combines them by weighted sum, keeping the score *magnitude* that pure rank fusion throws away. It's rag-cpp's measured default because it beats Reciprocal Rank Fusion (RRF) on NDCG.

**One embed round-trip, not N.** A single search fans into several probes — the query itself, RAG-Fusion paraphrases, a HyDE passage. Their dense embeddings are batched so expansion and HyDE don't multiply the network latency of a search the way N serial round-trips would.

### 2. Pseudo-relevance feedback — recover the words you didn't type *(default-on)*

A bare query rarely uses the exact vocabulary the docs use. agentty runs an initial BM25 pass, treats the top hits as *pseudo-relevant*, harvests their most **discriminative** terms (feedback frequency × rarity), and fuses in a second, down-weighted BM25 probe over `{query + those terms}`. A chunk that matches both the literal query *and* the expanded vocabulary is reinforced.

This is the classic **RM3** technique — deterministic, sub-millisecond, no model, no network — which is why it's **on by default**. Disable with `AGENTTY_RAG_PRF=0`.

### 3. Contextual retrieval — a chunk knows where it lives *(default-on)*

Before indexing, each chunk is **situated in its document** — Anthropic's 2024 "contextual retrieval" — so a fragment that lost its heading still ranks. With no LLM contextualizer wired, the engine uses a deterministic **extractive** fallback (document title + heading breadcrumb, e.g. `guide.md › Installation › Linux`) that needs no model and never fails. Either way the context is indexed on both the keyword and embedding sides, so a chunk that says "run the installer" is findable by "linux install" even though neither word appears in its body. This is an index-time cost only; it's a measured recall win and is **on by default** (`AGENTTY_RAG_CONTEXTUAL=0` disables).

### 4. Re-ranking — precision at the top

First-pass fusion is recall-oriented and noisy at the very top, so the candidate pool is re-scored by a **feature-fusion reranker** *(default-on)* — the same deterministic, model-free reranker rag-cpp's built-in pipelines use. It combines cheap signals the first pass ignores: exact query-term coverage, phrase proximity, title/path match, and (when embeddings are present) calibrated cosine similarity. Pure C++, zero network, fully reproducible.

### 5. MMR diversification — no near-duplicates *(default-on)*

Maximal Marginal Relevance greedily keeps hits that are both relevant *and* different from what's already selected, so three overlapping windows of the same paragraph don't crowd out three distinct answers.

### 6. Parent-document stitch — read it in context *(default-on)*

Small chunks retrieve *precisely* but read out of context. After ranking, each surviving chunk is stitched back into its **adjacent sibling chunks** from the same document, so the model sees the precise hit *inside* its surrounding prose — without widening the retrieval probe. Pure in-memory, no network. Disable with `AGENTTY_RAG_STITCH=0`.

### 7. GraphRAG expansion — retrieval over the document graph *(default-on)*

A flat chunk index treats every passage as an island. Real knowledge bases aren't flat: documents **link** to each other, and documents about the same thing **share vocabulary**. agentty builds that structure into an explicit **document graph** and does [GraphRAG](https://microsoft.github.io/graphrag/)-style **local multi-hop expansion** over it — entity graph → communities → graph-aware retrieval — with every ingredient made **deterministic and free** (no LLM).

**The graph** (nodes = documents) is built once per corpus and memo-cached — invalidated only when the corpus itself changes. It has two kinds of edge:

- **Link edges** — resolved markdown links (`](other-doc.md)`). An author-curated relevance signal most engines throw away. Ambiguous targets (a filename owned by two docs) are skipped rather than guessed, so a wrong edge never poisons the graph.
- **Entity edges** — the LLM-free half of GraphRAG's entity graph. Each doc's **salient entities** are its top terms by tf·idf that are also *rare* corpus-wide (they discriminate). Two documents that share ≥2 such entities get an edge — related even when the author drew no link (`quantizer.md` ↔ `codebook.md` connect because they co-mention the same rare identifiers).

**Authority.** [PageRank](https://en.wikipedia.org/wiki/PageRank) runs over the *link* graph only (being cited by an author is an endorsement; entity similarity is symmetric, not a vote). It supplies a deterministic authority prior — hub docs win ties.

**Communities.** Deterministic label propagation over the *union* of link + entity edges (a dependency-free stand-in for Leiden clustering) partitions the corpus into topic clusters.

**Retrieval** expands around the top hits through four tiers, in priority order, each ranked internally by PageRank, all scored *below every direct hit* so expansion is supporting material and never displaces a real answer:

| Tier | Signal | Why it helps |
|---|---|---|
| **Outbound links** | docs a hit links to | the hit vouches for them |
| **Backlinks** | docs that link *to* a hit | usually the overview that contextualizes it |
| **Entity neighbours** | docs sharing the hit's rare entities | related-by-topic even with no authored link |
| **Community hub** | highest-PageRank doc in the top hits' shared community | the authority for the neighbourhood you're asking about |

All of it is deterministic, in-memory, and needs no model. Disable with `AGENTTY_RAG_GRAPH=0`.

### 8. Corrective grading (CRAG) — know how good the answer is *(default-on)*

After the pipeline runs, a **corrective evaluator** ([CRAG](https://arxiv.org/abs/2401.15884)) grades the result set and produces a single **calibrated confidence**. When the first pass looks weak, it can strip stopwords, widen the pool, and retry — recovering hits when the query was buried in conversational phrasing. That confidence is the signal agentty uses everywhere downstream — most importantly it's the gate for **unprompted proactive injection** (see the proactive path below). Disable with `AGENTTY_RAG_CORRECT=0`.

## Conversation-aware retrieval *(default-on)*

The query you type is not always the query you mean — especially mid-conversation. The default-on **query-rewriting boosters** below (multi-query and HyDE) widen the probe set without ever replacing your original query, so recall can only rise: a vague follow-up ("how does **it** handle errors?") or a compositional question ("how the auth flow works **and** how the sandbox blocks paths") is rephrased into several probes that each retrieve on their own strength and fuse back into one ranked set. Because they lean on a small local generator, they cost nothing beyond a localhost round-trip and silently no-op when Ollama is absent.

## The learning loop *(default-on)*

agentty is the rare retrieval engine that **sees what happens after it answers** — and it uses that. Every passage `search_docs` surfaces counts a *use*; when the agent follows up by `read`ing the file a passage pointed at, that counts a *win* (an implicit relevance judgment — the passage pointed somewhere worth acting on). The Beta-smoothed win-rate per passage persists to `.agentty/rag_feedback.tsv` (human-inspectable TSV) and folds back into ranking as a bounded nudge — passages that repeatedly help rise, chronic noise sinks, and near-ties resolve toward what has actually worked *in this workspace*. Retrieval gets better the more you use it. `AGENTTY_RAG_LEARN=0` disables; delete the TSV to forget.

## Measure it: `agentty rag-bench`

A pipeline you can't measure is a pile of vibes. `agentty rag-bench [dir]` benchmarks the funnel **on your own corpus**, offline, in milliseconds. It synthesizes **known-item queries** from sampled chunks — each query is a chunk's most discriminative terms by tf×idf, so the chunk it came from is the known gold answer (deterministic and reproducible, no LLM needed). Then it runs the retrieval **ladder** — `bm25-only` → `hybrid+prf` → `+feature-rerank` → `+mmr` — over every query and reports **recall@k, MRR, and nDCG** per rung. Because each rung adds exactly one stage, a metric that *drops* at a rung points at the stage worth tuning, and every `AGENTTY_RAG_*` knob can be set against numbers instead of guesses.

Gold matching is **coverage-based**, not exact-identity: a returned chunk counts as a hit when it *covers* the gold chunk's region (same document + overlapping line span). This matters because the reranker deliberately collapses overlapping-window siblings into one survivor — which contains the gold region but may start on a different line. Scoring that as a miss would falsely flag the reranker as a regression; coverage matching measures the retrieval outcome the user actually gets.

Known-item synthesis exercises the funnel's *mechanics*; run it with a local embedder up to also exercise the dense (embedding) rungs, which is where semantic paraphrase wins show.

## Query-rewriting boosters *(default-on, local)*

Two of the strongest recall techniques need a *generative* model — but agentty runs them on a **small local Ollama model** (`AGENTTY_RAG_GEN_MODEL`, default `qwen2.5:0.5b`) on the very same Ollama that serves embeddings, so they add **zero cloud cost**. If that model isn't reachable they silently no-op and plain hybrid still runs; they're also **skipped on the latency-sensitive proactive pre-turn path** so unprompted retrieval stays fast. Both help **every** knowledge configuration — docs, skills-only, memory-only, MCP, or any mix — because they feed extra probes into the same source-agnostic fusion.

- **Multi-query / RAG-Fusion expansion** (`AGENTTY_RAG_EXPAND`, default on) — the local model rewrites your query into several alternative phrasings; each is retrieved and all results are fused. Vocabulary mismatch on any one phrasing stops being fatal. Disable with `AGENTTY_RAG_EXPAND=0`.
- **HyDE — Hypothetical Document Embeddings** (`AGENTTY_RAG_HYDE`, default on) — a question and its answer can sit far apart in embedding space. The local model drafts a short *answer-passage* for your query; embedding that draft lands the probe near the real passages. The answer needn't be correct — only look like one. Composes with, and is independent of, query expansion. Disable with `AGENTTY_RAG_HYDE=0`.

## The proactive path

Beyond the explicit `search_docs` tool, agentty can retrieve **before you even ask**. When your message looks knowledge-shaped, it runs the funnel pre-turn and injects a `<retrieved-context>` block (source-tagged, deduplicated across turns) into the prompt — grounding the answer in your docs/skills/memory without a tool round-trip. This is on by default (`AGENTTY_RAG_PROACTIVE=1`) and only injects above a **CRAG-calibrated** confidence bar (`AGENTTY_RAG_PROACTIVE_MIN`, default `0.35`).

Because this runs on the submit thread, it is bounded by a small fast-path **hedge** (`AGENTTY_RAG_PROACTIVE_BUDGET_MS`, default `250`). On a fast corpus the funnel finishes within the hedge and the grounding is injected inline — same turn, no perceptible delay. On a large or slow corpus — where the synchronous dense query-embed round-trip would overrun — the submit thread never blocks: the turn enters its normal streaming state (the status bar shows *retrieving context…* and the activity spinner animates, so the UI stays alive and never feels hung), and the stream launch is *held* behind a background retrieval that injects the `<retrieved-context>` block **the same turn**, the moment it lands. Grounding is always for the question just asked — never dropped, never deferred to a later, possibly-unrelated turn. If the user cancels (Esc) while retrieval is in flight, the late block is discarded. Set the hedge to `0` to always take the deferred (spinner-visible) path.

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

- **A production hybrid engine.** BM25, dense/HNSW, convex (TM2C2) fusion, the feature reranker, MMR, parent-stitch, PRF, the code-aware chunker, GraphRAG's document graph (PageRank, entity extraction, community detection), and CRAG grading all live in the **rag-cpp** library behind one adapter (`src/rag/adapter.cpp`). The only optional network hop is localhost Ollama.
- **One embed round-trip.** However many probes a search fans into — the query, RAG-Fusion paraphrases, a HyDE passage — their dense embeddings batch into a single `/api/embed` call, so expansion and HyDE never multiply network latency. Sources fan out through one `retrieve_multi` seam, so the batching reaches every knowledge configuration (docs, skills, memory, MCP).
- **Graceful degradation everywhere.** No embeddings → BM25-only. Ollama unreachable mid-search → the affected stage no-ops and retrieval continues. Generator down → HyDE/multi-query silently skip and plain hybrid still runs. Empty corpus, blank query, zero-k → empty result, never an error.
- **Deterministic where it counts.** Every non-generative stage — hybrid, fusion, feature-rerank, MMR, parent-stitch, PRF, the whole GraphRAG graph, its PageRank and communities, and CRAG grading — is deterministic given the corpus, so results are reproducible and `rag-bench` numbers are stable. Only the local-generator boosters (HyDE, multi-query) introduce a model.
- **Warm across sessions.** The built index is persisted to `.agentty/rag_docs.ragdb` (`AGENTTY_RAG_PERSIST`, on by default), so a later session opens warm instead of re-embedding the corpus. The document graph is memo-cached per corpus shape, and the learning-loop feedback persists to `.agentty/rag_feedback.tsv` — so expensive work is paid once and survives restarts.
- **Sensible defaults.** Every stage runs by default and every model call is local, so the default `search_docs` is fast, predictable, and cloud-free.
- **One seam for RAG and MCP.** From the model's view a docs folder and an MCP server are the same thing — a knowledge source behind one interface — so they fuse identically.
