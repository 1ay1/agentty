---
title: Performance
description: How agentty stays fast — input-to-photon latency, render caching, connection warmth, and speculative tool execution.
nav_section: Advanced
nav_order: 75
slug: performance
---

agentty is built to feel instant. A terminal agent's speed is two separate problems — the **local pipeline** (how fast a keystroke becomes a pixel) and the **wall-clock of a turn** (how long real work takes) — and agentty attacks both.

## Input to photon

Every keystroke travels the same path: terminal read → event parse → reducer → view build → layout → render → diff → terminal write. On a modern machine that whole round trip is around **1 ms** — the cursor moves the same frame you press the key.

A few things keep it there:

- **Event-driven loop.** With no active animation the run loop is purely event-driven — it renders on input, a message, or a timer, never on a fixed clock. An idle screen does *zero* work and *zero* CPU.
- **Visual-hash gate.** Before every render agentty hashes the visible model state. If nothing the user can see changed, the frame is skipped entirely — no layout, no paint, no bytes on the wire. Time-driven animations (the spinner, the streaming cursor, the welcome sigil) bucket their state into the hash so each visible step still advances it.
- **Cross-frame cell cache.** Settled content — prior turns, tool cards, the welcome wordmark — is keyed by a stable content hash. Once painted, it re-blits from a cell cache instead of re-running layout and paint. A 3000-row transcript costs the same per frame as a short one.
- **No per-frame I/O.** The view function is pure. It never reads a file, never touches the disk, never does crypto. Anything that would (a credential check for a sign-in badge, a config file for a picker row) is cached on the file's modification time, so the hot path pays a nanosecond stat instead of a read-and-parse.

## Streaming that never chokes

Model output arrives in bursts. agentty paces those bytes onto the screen at a smooth character rate rather than dumping each network chunk, so text reveals like typing regardless of how the server batches it. The first token gets a fast path so the model starts "talking" the instant its first byte lands, and the reveal animation runs on its own wall clock so a slow frame never makes it stutter.

Resuming a huge thread is bounded too: agentty rehydrates only the last couple of screens of a conversation into the live canvas — everything older is already in your terminal's native scrollback — so opening a thread with thousands of messages is as fast as a short one.

## Connection warmth

The slowest part of a request is often the handshake nobody sees. agentty opens a TCP + TLS + HTTP/2 connection to the active provider *while you're still typing*, so your first message skips the dial entirely. TLS session tickets let reconnections resume without a full handshake. If a session sits idle long enough for the pooled connection to lapse, the next keystroke silently re-warms it — so a request after a coffee break is as fast as one mid-flow.

Prompt caching is wired end-to-end for Anthropic: the stable prefix of your conversation (system prompt, tool definitions, prior turns) is marked cacheable, so the model re-reads it from cache instead of re-pricing it every turn. On a long session that's the difference between a fast first token and a slow one.

## Speculative tool execution

The biggest lever in an agent session isn't milliseconds — it's the number of model round-trips and how much of each turn is spent waiting on tools. agentty overlaps that work:

- **Parallel batches.** Independent tool calls in one turn run concurrently; only genuine conflicts serialize. See [Tools → parallel & speculative execution](/docs/tools#parallel--speculative-execution).
- **Speculative reads.** A read-only tool starts the moment its arguments finish streaming — while the model is still writing the rest of the turn — so a multi-tool turn hides seconds of file and search time inside the model's own generation.

Neither changes the result. Tool-heavy turns simply finish sooner.

## Compiled for the machine

- **Optimized release builds** ship with link-time optimization and a stripped symbol table. A scripted profile-guided-optimization workload (`scripts/pgo-train.sh`) can train the binary on the real hot paths — boot, typing, picker navigation, redraw — for another single-digit-percent win on the streaming and layout paths.
- **Allocator.** The render and parse paths are allocation-heavy; local builds can route allocation through mimalloc (`-DAGENTTY_USE_MIMALLOC=ON`) for a measurable keystroke-latency improvement.

## Measuring it yourself

Set `AGENTTY_CACHE_PROF=1` and agentty appends one line per turn to `/tmp/agentty-cache-prof.log` with three numbers worth watching:

- **cache hit ratio** — the fraction of your prompt prefix the model served from cache. A high ratio means fast, cheap turns; a sudden drop on an interior turn means something invalidated the cached prefix.
- **TTFT** (time to first token) per model — the wall-clock gap from launching a request to the first content byte.
- **batch width** — how many tool calls the model emitted in a single round-trip. Wider batches mean fewer round-trips per task.

The maya render engine also honors `MAYA_FRAME_PROF=/path/to/log`, which records per-frame build / layout / paint timings — useful when profiling the view pipeline directly.
