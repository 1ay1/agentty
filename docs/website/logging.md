---
title: Logging & diagnostics
description: agentty's structured log — leveled channels, atomic file output, a crash-time flight recorder, and the full API wire dump for debugging providers.
nav_section: Advanced
nav_order: 55
slug: logging
---

When something misbehaves — a custom host that won't respond, a provider that errors mid-stream, a swallowed exception — agentty can tell you exactly what happened. There are two tools: the **structured log** (`AGENTTY_LOG`) for leveled, channeled events, and the **raw API dump** (`AGENTTY_DEBUG_API`) for byte-level request/response bodies.

## Quick start

```bash
# Everything, to the default log file:
AGENTTY_LOG=debug agentty

# One subsystem wide open (e.g. the HTTP/SSE wire), warnings elsewhere:
AGENTTY_LOG=warn,wire=trace agentty

# Full request + response bodies for a misbehaving provider:
AGENTTY_DEBUG_API=1 AGENTTY_DEBUG_FILE=~/.agentty/api.log agentty
```

## `AGENTTY_LOG` — the structured log

The filter is RUST_LOG-style: a default level plus optional `channel=level` overrides, comma-separated.

```bash
AGENTTY_LOG=debug                    # every channel at debug and above
AGENTTY_LOG=wire=trace               # just the wire channel, at trace
AGENTTY_LOG=warn,wire=trace,auth=debug
                                     # default warn; wire and auth louder
AGENTTY_LOG=off                      # silence (the default when unset)
```

**Levels** (low → high): `trace` · `debug` · `info` · `warn` · `error`. A filter of `warn` passes `warn` and `error`; `off` silences a channel entirely.

**Channels** — one per subsystem:

| Channel | Covers |
|---------|--------|
| `wire` | HTTP/SSE transports, request/response lifecycle |
| `auth` | OAuth flows, key resolution, account switching |
| `persist` | settings / threads / memory disk I/O |
| `tool` | tool dispatch, permissions, sandbox |
| `ui` | reducer / view anomalies |
| `rag` | the retrieval engine |
| `mcp` | MCP bridge + plugins |
| `acp` | ACP server / adapter |
| `smart` | Smart Mode routing decisions |
| `net` | sockets, TLS, proxy, prewarm |
| `general` | uncategorised (swallowed exceptions land here) |

### Where it writes

- `AGENTTY_LOG_FILE=<path>` sets the file explicitly.
- With just `AGENTTY_LOG` set (no file), agentty logs to `$XDG_STATE_HOME/agentty/agentty.log` (or `~/.agentty/agentty.log`). So `AGENTTY_LOG=debug agentty` just works — no path needed.
- The file is append-only and rotates once at startup if it exceeds **32 MB** (the previous log becomes `.old`), so an always-on log can't grow unboundedly.

### The line format

```
2026-08-28T01:23:45.678 +0012345ms 1a2b W wire    openai.stream: connect refused host=localhost:8080
└── wall clock ────────┘ └ mono ─┘ tid  L channel  site: message
```

- **wall clock** — human-readable, millisecond precision.
- **`+…ms`** — monotonic time since process start, for reading event *pacing* without wall-clock math.
- **`tid`** — a short thread tag, so interleaved worker output is separable.
- **`L`** — level char (`T`/`D`/`I`/`W`/`E`).
- **channel** and **site** (`openai.stream`, `persistence.save`) locate the emitter.

It's one line per event, logfmt-ish and grep-first:

```bash
grep ' E ' ~/.agentty/agentty.log            # every error
grep 'wire ' ~/.agentty/agentty.log          # everything on the wire
```

### Performance

The log is engineered to be free when off. Each call site is gated by a single atomic load *before* its message is formatted — a disabled statement costs about a nanosecond and allocates nothing. When enabled, each event formats into a stack buffer and lands as a single atomic `write(2)` (no lock on the write path). Leaving `AGENTTY_LOG` unset has zero cost.

## The flight recorder

Even with file logging **off**, agentty keeps the last ~256 significant events (`warn` and above) in a small in-process ring. If agentty crashes (SIGSEGV / SIGABRT), the crash handler dumps that ring to stderr right after the backtrace:

```
=== agentty: SIGSEGV (segmentation fault) ===
  <backtrace frames>
=== agentty flight recorder (last events, oldest first) ===
2026-08-28T01:23:44.101 +0012310ms 1a2b W wire  openai.stream: retry 3/6 …
2026-08-28T01:23:45.678 +0012345ms 1a2b E persist save_thread: disk full
==================
```

So every crash report ships with *what was happening right before it* — at essentially zero steady-state cost. Redirect stderr to capture it: `agentty 2> crash.log`.

## `AGENTTY_DEBUG_API` — raw wire dump

For debugging a specific provider (a custom host that returns something unexpected, a streamed error, a malformed frame), the structured log's `wire=trace` gives you the lifecycle, but `AGENTTY_DEBUG_API` gives you the **full bytes**:

```bash
AGENTTY_DEBUG_API=1 AGENTTY_DEBUG_FILE=~/.agentty/api.log agentty
```

It writes the request line (method, host, port, path, model), the first 4 KB of the request body, the response status, and every response chunk (first 2 KB each) — for **every** provider wire (Claude, OpenAI-compatible, local). If `AGENTTY_DEBUG_FILE` is unset it writes to `./agentty-api.log`.

This is the tool to reach for when a local server "locks": you'll see the exact path agentty dialed and the exact bytes the server answered — a missing `/v1`, a chat-template rejection, a 404 for an unloaded model.

:::note Two tools, on purpose
`AGENTTY_LOG` logs *events* (structured, leveled, ring-buffered). `AGENTTY_DEBUG_API` dumps *raw bytes* (full bodies, unstructured). Use `wire=trace` to understand the flow; add `AGENTTY_DEBUG_API=1` when you need the actual payloads.
:::

## Legacy variable

`AGENTTY_DEBUG_LOG=<path>` (the older single-file debug var) still works: it sets the log file *and* implies `AGENTTY_LOG=debug` when `AGENTTY_LOG` is unset. Existing scripts keep working; new setups should prefer `AGENTTY_LOG`.

## Reporting a bug

For a good report, run with logging on and attach the file:

```bash
AGENTTY_LOG=debug AGENTTY_LOG_FILE=/tmp/agentty.log agentty
# reproduce the issue, then attach /tmp/agentty.log
```

If agentty crashed, include the stderr output (backtrace + flight recorder). For a provider/custom-host problem, add `AGENTTY_DEBUG_API=1` and attach the API log too.
