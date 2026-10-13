---
title: "I rewrote the Claude terminal agent in C++ and it cold-starts 50× faster"
date: 2026-06-07
author: agentty
tags: [performance, c++, deep-dive]
excerpt: "claude-code spends ~150 ms and 222 MB of Node just to print its version. agentty is one 9.6 MB binary that does it in ~3 ms. Here's exactly where the time goes, why a coding agent is a surprisingly good fit for C++26, and the numbers you can reproduce yourself."
---

# I rewrote the Claude terminal agent in C++ and it cold-starts 50× faster

Run this against the official client:

```
time claude --version
```

On my box that's about **150 ms** — and behind it sits a **222 MB** install
plus a Node runtime that has to spin up V8, walk a `require()` graph, and JIT
its way to printing six characters and a newline.

Now the same thing against agentty:

```
time agentty --version
# agentty 0.2.0
# real  0m0.003s
```

**~3 ms.** One **9.6 MB** static binary. No Node, no Python, no Electron, no
`npm install`. That's roughly **50× faster to cold-start** and **23× smaller on
disk** — for the exact same job: a Claude coding agent that lives in your
terminal and signs in with your existing Pro/Max subscription.

This post is the "where did the time actually go" teardown. No hand-waving —
every number here you can reproduce on your own machine in under a minute.

## The benchmark, in full

Same Arch box, same shell, same day, on my machine:

| | agentty (C++26) | claude-code (Node) |
|---|---|---|
| Cold-start `--help` | **~3 ms** | ~150 ms |
| `--version` | **~3 ms** | ~60 ms |
| Binary on disk | **9.6 MB** | 222 MB (+ Node runtime) |
| Install | `curl \| chmod +x` | `npm i -g` + Node |
| GC pauses mid-stream | None | V8 GC |

The 50× cold-start gap isn't a microbenchmark trick. It's structural. A Node CLI
pays for the runtime *every single invocation*: process start, V8 init, module
resolution, source parse, JIT warmup — before your code's first statement runs.
A static native binary pays none of that. The kernel `execve`s it, the dynamic
loader maps libc, and `main()` runs. There is no interpreter to boot.

## "But who cares about 150 ms?"

Cold-start latency is the tax you pay on *every* shortcut, every `git` hook,
every shell alias, every "quick question" you fire at the agent and `Ctrl-C` two
seconds later. 150 ms is over the threshold where a human perceives lag. 3 ms is
not. Multiply by how often you actually reach for the tool and the difference
stops being academic.

And cold start is just the visible tip. The one that bites during real work is
**GC pauses mid-stream**. When tokens are flowing in from the API and V8 decides
to collect, your TUI stutters. A native binary with no garbage collector has
nothing to pause for — the redraw loop is a `poll(2)` over the model stream and
your input fd, and every SSE chunk lands on the very next frame.

## Why a coding agent is a *good* fit for C++26

The instinct is "C++ for a chat client? masochism." But look at what an agent
loop actually is, and it's almost suspiciously well-suited to modern C++:

- **It's a state machine.** The whole app is a pure-functional update loop:
  `(Model, Msg) -> (Model, Cmd)`. The reducer is one `std::visit` over a closed
  sum of events. The compiler checks you handled every variant. There is no
  "unhandled action type" runtime surprise.
- **The dangerous parts are compile-time.** The permission matrix — which tools
  may touch the filesystem, the shell, the network — is a `constexpr` table
  guarded by `static_assert`. Change a policy cell wrong and **the build
  breaks**, not a test nobody runs, not production.
- **Strong types kill whole bug classes.** `ToolCallId`, `ThreadId`,
  `OAuthCode`, `PkceVerifier` are distinct newtypes. Swapping two arguments is a
  compile error, not a 2 a.m. debugging session.
- **Subprocess is the hard part, and C++ is good at it.** `posix_spawn` +
  `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX,
  `CreateProcessW` on Windows. No `popen` quoting hazards, no shelling out to GNU
  `timeout`.

C++26 (with the maya TUI engine doing the rendering) turns out to be a calm,
boring place to write this. The unsafe stuff is unrepresentable; the fast stuff
is free.

## Where the 9.6 MB goes (and why it's not 222)

A single static binary links OpenSSL + nghttp2 + libstdc++ into one file. There
is no runtime to ship alongside it because *it is the runtime*. The HTTP/2 + SSE
stack to talk to Anthropic is in-house on top of nghttp2 + OpenSSL — no
dependency tree to audit, nothing to `npm install`, no lockfile drift between
your laptop and the CI box and the air-gapped server.

`curl | chmod +x | run`. The same one line updates it. That's the whole install
story.

## The party trick: it runs on an air-gapped box, and inside Zed

Two things fall out of "single static binary" almost for free:

1. **`agentty airgap user@host`** runs the agent on a machine with no direct
   internet — your laptop relays the bytes over SOCKS5-over-SSH, with TLS pinned
   on the real upstreams end-to-end. One command. Try that with a 222 MB Node
   install you have to get onto the box first.
2. **`agentty acp`** speaks the [Agent Client Protocol](https://agentclientprotocol.com),
   so the *same binary* becomes a first-class agent panel inside Zed — streaming
   text, inline diffs, native permission prompts, session reload. Same engine as
   the TUI, just driven over JSON-RPC on stdio.

## Reproduce it yourself

Don't take my word for the 50×. Install it (this is the literal install
command, not a curated demo):

```
curl -fsSL https://agentty.org/install.sh | sh
```

Then:

```
time agentty --version
time claude --version   # if you have it, for the side-by-side
```

The source is MIT and reads like a single function — one `std::visit` reducer, a
single render function, a permission matrix you can audit in one sitting.
[Browse the repo](https://github.com/1ay1/agentty), file an issue, or send a PR.

If you've ever closed a terminal agent because the startup lag broke your flow:
this one starts before you finish letting go of `Enter`.

Cold start is one piece of a bigger picture — see [Why terminal-first AI
feels faster than web-based tools](/blog/why-terminal-first-ai-feels-faster)
for the full latency chain, from DOM diffing to garbage-collector pauses,
that a browser-hosted tool can never architect its way out of.
