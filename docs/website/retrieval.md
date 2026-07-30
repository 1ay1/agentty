---
title: Retrieval (RAG)
description: How agentty's search_docs and search_code tools retrieve accurate, bounded context from local knowledge and source code.
nav_section: Tools
nav_order: 40
slug: retrieval
---

agentty provides two local retrieval tools:

- **`search_docs`** searches documentation, installed skills, learned memory, and optionally connected MCP resources.
- **`search_code`** searches source code by meaning when you do not know the exact identifier. Use `grep` for exact names and strings.

Retrieval is available in Linux, macOS, MinGW, and official MSVC Windows builds. With no embedding server, it uses BM25 immediately. If a compatible Ollama embedding model is reachable on localhost, agentty adds dense semantic retrieval.

## Production defaults

The default profile is intentionally conservative:

1. Structural/contextual chunking.
2. BM25, plus Ollama embeddings when a short availability probe succeeds.
3. Weighted Reciprocal Rank Fusion.
4. A deterministic feature reranker.
5. MMR diversification.
6. Adjacent-hit deduplication/stitching.
7. Query-focused passage compression under a strict aggregate output budget.

Expensive or corpus-sensitive stages are **opt-in**, not taxes on every query:

- Pseudo-relevance feedback: `AGENTTY_RAG_PRF=1`
- GraphRAG: `AGENTTY_RAG_GRAPH=1`
- Corrective grading: `AGENTTY_RAG_CORRECT=1`
- Multi-query expansion: `AGENTTY_RAG_EXPAND=1`
- HyDE: `AGENTTY_RAG_HYDE=1`
- Implicit learning: `AGENTTY_RAG_LEARN=1`
- Proactive pre-turn injection: `AGENTTY_RAG_PROACTIVE=1`

This profile favors predictable latency and token economy. Enable optional stages only after measuring them on your corpus with `agentty rag-bench` and representative real queries.

## What gets indexed

| Source | Default | Configuration |
|---|---:|---|
| Docs folder | Auto-discovered | `AGENTTY_DOCS_DIR`, then `./docs`, then `./.agentty/knowledge` |
| Installed skills | On | `AGENTTY_RAG_SKILLS=0` disables |
| Learned memory | On | `AGENTTY_RAG_MEMORY=0` disables |
| MCP resources | Off | `AGENTTY_RAG_MCP=1` enables resource reads |
| Workspace source code | On demand | Indexed separately by `search_code` |

MCP indexing is opt-in because reading a resource can involve server I/O. MCP-backed indexes are not persisted across sessions, preventing stale session resources from leaking into a later run.

## Dense retrieval and graceful fallback

BM25 needs no setup. To enable semantic retrieval:

```bash
ollama pull nomic-embed-text
ollama serve
```

The default endpoint is `127.0.0.1:11434`. Override it with:

```bash
export AGENTTY_OLLAMA_HOST=127.0.0.1:11434
export AGENTTY_EMBED_MODEL=nomic-embed-text
```

At startup, agentty performs one bounded embedding probe. If the server or model is unavailable, the session stays BM25-only instead of paying a long failed network timeout on every search. The result header reports `bm25` or `hybrid` truthfully.

`AGENTTY_RAG_BM25_WEIGHT` and `AGENTTY_RAG_DENSE_WEIGHT` control weighted RRF directly. Setting dense weight to zero removes dense influence; the knobs are not ignored by a separate fusion alpha.

## Persistence and freshness

The docs/skills/memory index is stored at:

```text
.agentty/rag_docs.ragdb
.agentty/rag_docs.ragdb.meta.json
```

The manifest records the docs root, indexable file paths, size/mtime stamps, skill and memory content fingerprints, chunking configuration, and embedding profile. A later process opens the database directly when the manifest still matches.

Freshness scans:

- prune `.git`, build outputs, dependency trees, virtual environments, and caches before descending;
- inspect only extensions and sizes the loader can index;
- include relative paths, so renames invalidate the index;
- fingerprint skill and memory content, not only record counts.

When a small number of docs changes, agentty removes and reindexes only those documents. A large change set triggers a clean rebuild. `search_code` uses the same incremental strategy and caps its workspace corpus at 4,000 source files of at most 256 KiB each.

## Code-aware chunks

`search_code` uses rag-cpp's source chunker. Recognized languages split around definitions rather than arbitrary prose windows, so a result normally contains a coherent function, class, or declaration. The code index is independent of the docs index; editing one source file updates that file rather than rebuilding the repository.

## Token economy

Retrieval output has an aggregate body budget of approximately 12 KiB by default—roughly 3,000 tokens, depending on content. Configure it with:

```bash
export AGENTTY_RAG_OUTPUT_BYTES=12288
```

Accepted values are clamped between 2 KiB and 64 KiB.

When a passage is too large, agentty selects the line window with the strongest query-term evidence and includes nearby context. It does not blindly retain the beginning of a giant file. The budget applies across all returned passage bodies, not independently to each result.

The requested `k` is still honored when enough distinct passages exist. Optional corrective grading no longer silently collapses a broad `k=10` request to three strips.

## Proactive retrieval

Automatic pre-turn context injection is off by default. The explicit `search_docs` tool is normally more economical because the model calls it only when project knowledge is needed.

Enable proactive grounding with:

```bash
export AGENTTY_RAG_PROACTIVE=1
```

When enabled, the app runs exactly one retrieval on an isolated worker, displays `retrieving context…`, and launches the model after retrieval settles. It does not race a synchronous hedge against a duplicate background query. Injected context grounds that turn only; older proactive blocks are removed from later wire payloads so they are not repeatedly charged.

## Optional advanced stages

### PRF

`AGENTTY_RAG_PRF=1` expands a query from top lexical candidates. It can improve vocabulary mismatch, but it can also drift toward an initially wrong result. It is off until explicitly enabled.

### GraphRAG

`AGENTTY_RAG_GRAPH=1` builds document links, similarity edges, communities, and local graph expansion. Graph construction can be quadratic in document count, so it is unsuitable as an unconditional default. Base and graph results are fused by rank because their raw score scales differ.

### Corrective grading

`AGENTTY_RAG_CORRECT=1` grades retrieval using normalized retrieval evidence plus lexical support. Semantic matches are not rejected merely because they paraphrase the query, and grading preserves the requested result count.

### Multi-query and HyDE

`AGENTTY_RAG_EXPAND=1` and `AGENTTY_RAG_HYDE=1` use a small local generator (`AGENTTY_RAG_GEN_MODEL`, default `qwen2.5:0.5b`). They are useful for research-heavy or ambiguous queries but add local model latency.

### Learning

`AGENTTY_RAG_LEARN=1` enables the experimental file-open feedback loop stored in `.agentty/rag_feedback.tsv`. It is off by default because a subsequent file read is not an attributable relevance judgment for skills, memory, or passages that already answered the question.

## Measuring retrieval

Run:

```bash
agentty rag-bench docs
```

The benchmark reports indexing time and a deterministic ladder of recall@k, MRR, nDCG@10, and milliseconds/query. A metric drop after adding a stage is evidence to leave that stage disabled or tune it.

Known-item queries measure retrieval mechanics and are intentionally lexical. For product accuracy, supplement them with a small labeled set of real questions, paraphrases, broad surveys, and expected source paths.

## Provenance

Every passage retains a source and path:

```text
docs:guide/auth.md:20-44
skill:release-checklist
memory:fact-id
mcp:file://resource-uri
code:src/auth/token.cpp:80-126
```

Use the provenance to cite, open, or verify retrieved information rather than treating it as ungrounded model knowledge.
