---
title: Retrieval (RAG)
description: How agentty's search_docs and search_code tools find the right docs and code, how to turn on semantic search in two commands, and how to check it's working.
nav_section: Tools
nav_order: 40
slug: retrieval
---

agentty has two local search tools the model can call:

- **`search_code`** finds code by what it does ("where are retries handled") when you don't know the name. For exact names and strings the model uses `grep`.
- **`search_docs`** searches your docs folder, your installed skills, and facts you asked it to `remember`.

Both work out of the box with keyword search. Everything runs on your machine; no code leaves it.

## Quick start: turn on semantic search

Keyword search works with no setup. For search by meaning, run [Ollama](https://ollama.com) and pull an embedding model:

```bash
ollama pull qwen3-embedding:0.6b    # best for code, ~640 MB
# or: ollama pull nomic-embed-text  # smaller, ~270 MB
```

That's it. agentty finds Ollama on `127.0.0.1:11434` by itself and prefers `qwen3-embedding:0.6b` when it's there. The first search in a repo builds the index (from about 30 seconds to a few minutes on a large repo); after that it loads in about a second and updates only the files you changed.

Check it's on:

```bash
agentty diagnostics
```

The `retrieval (embeddings)` section should say `probe: ok`. Every search result also says `hybrid` (keyword + meaning) or `bm25` (keyword only) in its header.

To pick a different model or a remote server, type `/settings` and open **Proactive retrieval (RAG)** (the Retrieval pane), or set:

```bash
export AGENTTY_EMBED_MODEL=nomic-embed-text
export AGENTTY_OLLAMA_HOST=gpu-box.lan:11434
```

## How well it works

agentty measures its own search on a judged set of real questions about its own code and docs (`tests/rag_eval`, run with `agentty rag-eval <file>`). "Right file first" means the file that answers the question is the top result:

| Search | Setup | Right file first | Right file in top 8 |
|---|---|---:|---:|
| `search_code` | keyword only | 57-60% | 92-95% |
| `search_code` | + `nomic-embed-text` | 60-63% | 88-90% |
| `search_code` | + `qwen3-embedding:0.6b` | 63-78% | 93-97% |
| `search_docs` | + embeddings | 70-85% | 93-97% |

Each range covers the main and held-out halves of the set (60 + 41 code questions, 30 + 20 docs questions).

What makes code search good: identifiers are split into words (`onClipboardRead` matches "clipboard read"), source files rank above tests and docs unless you ask about tests or docs, a header pulls its `.cpp` up with it, and results spread across files instead of returning one file eight times.

## If results look wrong

| Symptom | Cause | Fix |
|---|---|---|
| Results say `bm25`, never `hybrid` | No embedding server, or the model isn't pulled | `ollama pull qwen3-embedding:0.6b`, then `agentty diagnostics` |
| `diagnostics` says `invalid: host ...` | A typo in the Retrieval pane (e.g. `127.0.0`) | Fix the host there, or delete the `rag` keys in `~/.agentty/settings.json` |
| `search_docs` finds nothing | No docs folder | Put docs in `./docs` or `./.agentty/knowledge`, or set `AGENTTY_DOCS_DIR` |
| First search is slow | The index is being built | Only the first time per repo and model |
| Wrong file comes first | The query matches a sibling file better | Ask with words from the file's purpose; `grep` for exact names |

## What is on by default

1. Code-aware chunking: code splits at functions and classes, docs at headings.
2. Keyword search (BM25), plus embeddings when the startup check reaches a model.
3. Score fusion of the two (convex by default; `AGENTTY_RAG_FUSION=rrf` for weighted reciprocal-rank fusion).
4. A deterministic reranker, plus the code-specific rules above for `search_code`.
5. Diversity (MMR), duplicate removal, and stitching of adjacent hits.
6. Query-focused trimming under a strict total size budget.

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

At startup agentty runs one quick embedding check. If the server or model is unavailable, the session stays keyword-only instead of paying a failed network timeout on every search. The check waits up to 30 seconds for Ollama to load a model, but a server that isn't running fails at once.

With the backend on **Auto** (the default) and the model left at its default, agentty tries `qwen3-embedding:0.6b` first and uses `nomic-embed-text` when that isn't pulled. A model or backend you set yourself is used as is.

Models trained with query/document prefixes (nomic, e5, mxbai, arctic) get them automatically; `AGENTTY_EMBED_PREFIX=0` turns that off. Other backends (an OpenAI-compatible endpoint, llama.cpp, or an in-process GGUF/ONNX model) are picked in the Retrieval pane.

When a search returns **no hits**, the result says why — e.g. `no matches (bm25)` (it ran, nothing was relevant, and the meaning half was off), or `no matches (no docs indexed — set AGENTTY_DOCS_DIR …)` when there was nothing to search.

`AGENTTY_RAG_BM25_WEIGHT` and `AGENTTY_RAG_DENSE_WEIGHT` set the keyword/meaning balance in `rrf` fusion mode; the default convex mode adapts the balance per query instead.

## Persistence and freshness

Indexes are saved and reopened, never rebuilt on every start. They live with the project, so each checkout has its own:

```text
<project>/.agentty/cache/rag_docs.<id>.ragdb   # docs, skills, memory
<project>/.agentty/cache/rag_code.<id>.ragdb   # search_code
```

`AGENTTY_RAG_DIR` moves them (see [Environment](/docs/environment)).

`<id>` names the embedding model, so switching models keeps the old index for when you switch back. Each index stores per-file size/mtime stamps; when a later run sees changed files it updates only those, so one edited file costs seconds, not a rebuild. Changing the chunking, the docs folder, or the model starts a fresh index.

Freshness scans:

- prune `.git`, build outputs, dependency trees, virtual environments, and caches before descending;
- inspect only extensions and sizes the loader can index;
- include relative paths, so renames invalidate the index;
- fingerprint skill and memory content, not only record counts.

When a small number of docs changes, agentty removes and reindexes only those documents. A large change set triggers a clean rebuild. `search_code` uses the same incremental strategy and caps its workspace corpus at 4,000 source files of at most 256 KiB each.

## Code-aware chunks

`search_code` uses rag-cpp's source chunker. Recognized languages split around definitions rather than arbitrary prose windows, so a result normally contains a coherent function, class, or declaration. The code index is independent of the docs index; editing one source file updates that file rather than rebuilding the repository.

## Token economy

Retrieval is the one place agentty spends model-context tokens on your behalf, so the output path is aggressively budgeted. Five independent levers keep the spend proportional to how useful the retrieval actually is. All are on by default and env-tunable, and none of them change ranking—they only shape how many tokens the *already-ranked* passages consume.

### Aggregate output budget

Retrieval output has an aggregate body budget of approximately 12 KiB by default—roughly 3,000 tokens, depending on content:

```bash
export AGENTTY_RAG_OUTPUT_BYTES=12288   # clamped 2 KiB – 64 KiB
```

The budget applies across all returned passage bodies, not independently to each result.

### Relevance floor

The low-confidence tail of a retrieval is what the model ignores anyway, so paying flagship input price to inject it is waste. A passage survives only if its score is within a fraction of the top hit's score; the best hit is always kept.

```bash
export AGENTTY_RAG_RELEVANCE_FLOOR=0.30   # 0 keeps every hit
```

### Score-proportional (water-filling) budget

Instead of splitting the budget evenly by passage count—which handed a rank-8 hit at confidence 0.11 the same allowance as the rank-1 hit at 0.88—agentty splits bytes in proportion to score^gamma, with a per-passage floor so a kept tail hit still reads legibly. Confident passages get room to be complete; marginal ones get a tight excerpt.

```bash
export AGENTTY_RAG_BUDGET_GAMMA=1.5   # higher concentrates budget on the head
```

### Confidence-scaled total budget

A retrieval that barely clears the relevance bar should not reserve the same ~3,000 tokens as a slam-dunk. The *total* budget ramps linearly with retrieval confidence, from a floor fraction at low confidence to 100% at full confidence. Ungraded paths keep the full budget.

```bash
export AGENTTY_RAG_CONF_BUDGET_FLOOR=0.45   # min fraction of budget at low confidence
```

### Extractive passage compression

When a passage is too large, agentty compresses rather than truncates. For prose it runs a model-free, LLMLingua-style extractive pass: each sentence is scored by rare-term-weighted query overlap, the highest-scoring sentences are kept within budget, and they are re-emitted in original order with `…` gap markers. This drops irrelevant filler *between* relevant sentences—something a contiguous window cannot. Source code and config keep a contiguous line-window with the strongest query-term evidence, because inter-line contiguity is load-bearing for readability.

```bash
export AGENTTY_RAG_EXTRACTIVE=0   # disable extraction, force line-window
```

The requested `k` is still honored when enough distinct passages exist. Optional corrective grading no longer silently collapses a broad `k=10` request to three strips.

### Unprompted (proactive) spend

When proactive injection is enabled, the block is spent without the user asking, so it is capped independently and more tightly than the on-demand tool budget—about 6 KiB (~1,500 tokens) by default:

```bash
export AGENTTY_RAG_PROACTIVE_BYTES=6144   # clamped 1 KiB – 32 KiB
```

### Measured effect

On a realistic 161-file corpus, these levers together cut retrieval output from ~1,845 to ~1,605 estimated tokens per query (**−13%**) with **identical** ranking metrics (recall@10 1.000, MRR 0.968, nDCG@10 0.976). Savings scale with passage size: a corpus of small chunks that already fit under budget sees little change, because compression never fires. Retrieval is a fraction of a coding turn's total tokens—the stable system, tools, and history prefix dominate and is already discounted heavily by prompt caching—so treat this as a real but modest slice of the overall bill. Measure it on your own corpus (see below) rather than assuming.

## Proactive retrieval

Automatic pre-turn context injection is off by default. The explicit `search_docs` tool is normally more economical because the model calls it only when project knowledge is needed.

### The RAG picker (in-app)

Rather than juggling env vars, set proactive retrieval in the app: `/settings` → **Proactive retrieval (RAG)**. One decision, three modes:

| Mode | Behaviour |
|------|-----------|
| **On** | retrieve and inject context before **every** turn |
| **First turn only** | ground the **first** turn of a thread, then stay quiet |
| **Off** | no pre-turn injection (the `search_docs` / `search_code` tools still work) |

The choice is **persisted** in `settings.json`, so it sticks across sessions. A [forked thread](/docs/fork) can carry its own RAG mode, chosen in the fork picker, independent of the global setting. Setting `AGENTTY_RAG_PROACTIVE=1` (below) is the env-var equivalent of **On**; the picker is the everyday way in.

### The retrieved-context card

When proactive retrieval injects a block, it does **not** appear as if you typed it. It renders as its own quiet **retrieved-context** card — with the source list and a confidence bar reflecting the retrieval's grade — so you can always see *what* was pulled in and *how sure* the engine was. These synthetic blocks never leak into the composer's [[↑]]/[[↓]] history recall, and older ones are dropped from later wire payloads so you're not charged for them turn after turn.

### The retrieval funnel

Every `search_docs` / `search_code` result is headed by a compact **funnel** line showing how the candidate set narrowed — lexical/dense retrieval → fusion → dedup/autocut → the passages actually returned — plus whether the run was `bm25` or `hybrid`. It makes the ranking legible instead of a black box: you can see at a glance when a query found little, or when the relevance floor trimmed a weak tail.

### Enabling proactive retrieval via env

The env-var equivalent of the picker's **On** mode:

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

Two built-in tools, both offline.

`agentty rag-eval <cases.json>` scores search against questions with known answers, the way the tools actually run. agentty's own sets live in `tests/rag_eval/` (code and docs, each with a held-out half kept out of tuning). A cases file is a list of `{"q": "...", "gold": ["path", ...]}`; wrap it as `{"tool": "search_docs", "cases": [...]}` to test docs search. It prints the right-file-first rate (hit@1), the top-8 rate (hit@8), MRR, and every miss:

```bash
agentty rag-eval tests/rag_eval/agentty_code.json
AGENTTY_RAG_EVAL_ALL=1 agentty rag-eval my_cases.json   # every case's rank
```

Write twenty questions about your own repo with the file that answers each, and you have a check that a model or setting change really helped. The `rag_eval_guard` test runs agentty's sets keyword-only on every build and fails if search gets worse.

`agentty rag-bench` benchmarks `search_docs` on your docs folder:

```bash
agentty rag-bench docs
```

The benchmark reports indexing time and a deterministic ladder of recall@k, MRR, nDCG@10, and milliseconds/query. A metric drop after adding a stage is evidence to leave that stage disabled or tune it.

To measure **token cost** rather than ranking quality, add `AGENTTY_RAG_MEASURE=1`. This runs the benchmark's queries through the real retrieval output path—the one that applies the relevance floor, water-filling, confidence-scaled budget, and extractive compression—and reports the bytes and estimated tokens actually injected per query, plus a machine-readable `MEASURE` line:

```bash
AGENTTY_RAG_MEASURE=1 agentty rag-bench docs
```

To quantify any single lever, run it twice with the lever toggled and diff the `tokens=` field. For example, to isolate extractive compression:

```bash
AGENTTY_RAG_MEASURE=1 AGENTTY_RAG_EXTRACTIVE=0 agentty rag-bench docs   # off
AGENTTY_RAG_MEASURE=1                          agentty rag-bench docs   # on
```

Because every lever is env-tunable, no rebuild is needed to measure a configuration on your own corpus.

`rag-bench` builds its queries from the documents themselves, so it measures the mechanics. `rag-eval` with real questions measures what you actually care about.

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
