---
title: "How to run an AI coding agent completely locally with Ollama"
description: "Step-by-step: run a fully local AI coding agent with agentty + Ollama — no cloud, no API keys, no data leaving your machine. Works offline."
competitor: "Ollama coding agent"
verdict: "Point agentty at a local Ollama server and every request stays on your machine — a fully local, offline-capable coding agent with zero API keys."
updated: "2026-08-19"
---

# How to run an AI coding agent completely locally with Ollama

You can run agentty entirely offline against a local [Ollama](https://ollama.com) model — no cloud, no API keys, nothing leaves your machine. Here's how.

## 1. Install Ollama and pull a model

```
# install ollama (see ollama.com), then pull a coding-capable model
ollama pull qwen2.5-coder:7b
```

## 2. Install agentty

```
curl -fsSL https://agentty.org/install.sh | sh
```

## 3. Point agentty at Ollama

Configure the Ollama provider (host defaults to `http://localhost:11434`) and select your pulled model. agentty streams tokens from the local server exactly as it would from a cloud provider.

## 4. Code offline

Run `agentty` in your project. Because retrieval (RAG) is built in and runs locally too, only the relevant slices of your repo are fed to the model — which keeps local models fast and within their context window.

While Ollama is up, also pull an embedding model so code search works by meaning, not just keywords. agentty picks it up by itself:

```
ollama pull qwen3-embedding:0.6b
```

`agentty diagnostics` shows `probe: ok` under retrieval once it's on. See [Retrieval](/docs/retrieval).

## Why local?

- **Privacy** — source code never leaves your machine.
- **No cost** — no per-token billing.
- **Offline** — works on a plane or an air-gapped network.

## Tips

- Smaller models (7B) are fast; use agentty's Smart Mode to reserve a bigger local model for the hard steps.
- A GPU helps a lot for latency, but CPU-only works for smaller models.
- Local models get a short prompt that tells them to do the work with tools instead of pasting code or refusing a big task (see [the prompt local models get](/docs/providers#the-prompt-local-models-get)). A small model can still give up on a very large request, so ask for one piece at a time.

See the [providers docs](/docs/providers/) for full configuration.
