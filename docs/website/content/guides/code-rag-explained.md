---
title: "What is code RAG? Retrieval-augmented generation for coding agents"
description: "Code RAG means retrieving only the relevant parts of your repository for an AI coding agent instead of sending everything. Here's how it works and why it matters."
competitor: "code RAG"
verdict: "Code RAG retrieves only the relevant slices of your codebase for each request — using hybrid lexical + semantic search — so a coding agent is faster, cheaper, and more accurate than one that sends the whole repo."
updated: "2026-08-19"
---

# What is code RAG?

**Code RAG** (retrieval-augmented generation for code) is the technique of fetching only the *relevant* pieces of your codebase for a given task, then giving those to the model — instead of dumping the whole repository into the prompt.

## Why it matters

Large language models have a fixed context window and you pay per token. If a coding agent sends 50,000 tokens of code on every turn, it's slow, expensive, and the model's attention is spread thin. Code RAG solves all three by sending only what's relevant.

## How good code RAG works

agentty's retrieval engine is a strong reference implementation:

- **Hybrid search** — combines BM25 (lexical/keyword) with dense embeddings (semantic), so it matches both exact identifiers and conceptual queries.
- **Code-aware chunking** — splits along function/class boundaries rather than fixed line windows, so retrieved context is coherent.
- **Identifiers split into words** — `onClipboardRead` also matches "clipboard read", so a question in plain words finds the code.
- **HNSW ANN** — fast approximate nearest-neighbour search over the embedding index.
- **A code-aware reranker** — source ranks above tests and docs unless you ask about them, a header pulls its `.cpp` up, and results spread across files.
- **Opt-in extras** — GraphRAG expansion and a learning loop that nudges ranking toward files you open, off by default.

Turn on the meaning half with one command, then check it:

```bash
ollama pull qwen3-embedding:0.6b
agentty diagnostics      # "probe: ok" under retrieval
```

On agentty's own judged questions this puts the right file first about 63-78% of the time and in the top 8 about 95%. Details in [Retrieval](/docs/retrieval).

## The payoff

- **Lower cost** — often 80%+ fewer context tokens.
- **Faster responses** — smaller prompts.
- **Better answers** — the model sees relevant code, not noise.

## Try it

```
curl -fsSL https://agentty.org/install.sh | sh
```

agentty runs retrieval locally, with zero external dependencies. See the [retrieval docs](/docs/retrieval/).
