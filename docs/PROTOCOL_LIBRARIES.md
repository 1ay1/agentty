# Protocol libraries: no runtime, one copy, agentty drives

This is a rule, not a description. It governs mcp-cpp, acp-cpp and rag-cpp
(and claybin, which already complies). Where the code disagrees, the code is
the thing to fix. A change that breaks a rule here is not merged with a
"we'll fix it later"; it either complies or it changes this document first,
with a reason.

It extends [LAYERING.md](LAYERING.md) rule 5 ("the other submodules own no
runtime") from "they borrow a runtime the host installs" to "they contain no
runtime at all". The libraries are passive. agentty is the program.

## Contents

1. [The model](#1-the-model)
2. [The rules](#2-the-rules)
3. [What a library may contain](#3-what-a-library-may-contain)
4. [How each need is met without a runtime](#4-how-each-need-is-met-without-a-runtime)
5. [One copy: the shared JSON-RPC core](#5-one-copy-the-shared-json-rpc-core)
6. [What agentty provides](#6-what-agentty-provides)
7. [How it is enforced](#7-how-it-is-enforced)
8. [Migration plan and order](#8-migration-plan-and-order)
9. [Decision checklist for any change](#9-decision-checklist-for-any-change)
10. [Glossary](#10-glossary)

---

## 1. The model

```
agentty     the PROGRAM. Owns every thread, timer, lock, process and the
   │        decision of what runs when. Drives the libraries.
   ▼
 maya       agentty's only face onto the runtime (maya::guarded, pool,
   │        scope, delay_for, stop_group, published, platform::process ...)
   ▼
 jaal       the runtime itself.

 mcp-cpp ─┐
 acp-cpp ─┼─ PASSIVE LIBRARIES. Pure code. Called; never call out to a
 rag-cpp ─┘  thread, a clock, a lock or a process of their own.
 claybin     (already passive: it builds sandbox plans, agentty spawns them)
```

A library is a set of **values and functions**. Calling it does work on the
caller's thread and returns. It never:

- runs anything in the background,
- waits for anything (a timer, a reply, a pipe),
- shares mutable state between calls that two threads could reach,
- decides which thread anything runs on.

Everything that needs any of those lives in agentty, built on maya → jaal.
The library exposes the **state machine**; agentty supplies the **motion**.

The test that keeps this honest: *if every function in the library were
called from one thread, in some order, would the library behave correctly?*
It must. Concurrency is something agentty adds around it, never something
the library assumes or provides.

## 2. The rules

**R1. No runtime primitives in a library.** No `std::thread`,
`std::jthread`, `std::async`, `.detach()`, `std::mutex` (any kind),
`std::condition_variable`, `std::atomic`, `thread_local`,
`std::this_thread::sleep_*`, `std::stop_source`, `std::future`/`promise`,
`std::latch`/`barrier`/`semaphore`. Not even in a "standalone fallback". A
library that cannot run without a thread does not ship one; it documents what
the host must call.

**R2. No runtime interface in a library either.** No `Runtime`, `Executor`,
`set_runtime`, `spawn`, `sleep_for`, `parallel_for` hook that the library
calls out through. An injected runtime is still the library deciding *when*
to do concurrent work; it just borrows the threads. That is the thing we are
removing. The host calls in; the library never calls out to schedule.

The one exception is a pure **data-parallel kernel** whose work is already
split by the caller (see R6 and §4.6) — and even that takes the splitting as
an argument, not a global.

**R3. No shared mutable state.** No `static` (or global, or function-local
static) that changes after initialisation. A library object is owned by one
caller at a time. If two threads need it, agentty puts it inside a
`maya::guarded`, a `maya::published`, or behind one owning worker; the
library does not know.

Immutable statics (lookup tables, `constexpr`, a `const` set built once) are
fine.

**R4. No clocks.** A library never reads `steady_clock`/`system_clock`. Time
enters as an argument (`now`), a deadline enters as a value, and "has it
expired" is a pure function of the two. The caller owns the clock.

**R5. No processes or blocking I/O.** A library never spawns a child, opens
a pipe, or blocks reading one. It parses bytes it is given and produces bytes
to send. Spawning, reading and writing are agentty's (maya's
`platform::process` and the poll reactor). The one allowed I/O is reading a
**file the caller named**, synchronously, on the caller's thread, when that
is the library's whole job (rag-cpp loading a document).

**R6. Deterministic and re-entrant.** Same inputs, same outputs. No hidden
caches that change behaviour (a pure memo keyed on inputs is allowed if it is
owned by an object the caller holds, not a static). Callbacks the library
invokes are invoked synchronously, on the caller's thread, inside the call
that triggered them, and the library holds no lock while doing so.

**R7. One copy.** Code that exists in more than one library is a bug.
JSON-RPC framing, envelopes, the id/waiter table, the codec algebra, the
`Sum`/`Maybe`/`Newtype` vocabulary and coroutine plumbing live **once**, in
the shared core (§5). mcp-cpp and acp-cpp depend on it; neither keeps a
private copy.

**R8. The public API may break; the rules may not.** These libraries are
agentty's. When a rule and an existing API disagree, the API changes. We do
not keep a deprecated threaded path "for compatibility".

**R9. agentty is the only integrator.** Every place a library's work meets a
thread, a timer, a lock or a process is in agentty, in one module per
library, on maya names only. No agentty code outside those modules calls a
library's async-shaped API directly.

## 3. What a library may contain

| allowed | not allowed |
|---|---|
| plain types, value semantics, `std::variant`, `std::optional`, `std::expected` | `std::thread`, `jthread`, `async`, `detach` |
| pure functions, `constexpr`, templates | any mutex, `condition_variable`, `atomic` |
| objects whose methods mutate `*this` (owned by one caller at a time) | `thread_local`, mutable statics/globals |
| `std::function` callbacks invoked synchronously | a `Runtime`/`Executor`/`set_*` hook it calls out through |
| reading a caller-named file, synchronously | spawning processes, pipes, sockets, polling |
| taking `now` / a deadline as an argument | reading a clock |
| returning "what to do next" as data (an action list, a deadline) | sleeping, waiting, blocking on a future |
| a `parallel_for`-shaped kernel that takes its splitter as a parameter (§4.6) | deciding parallelism itself |

`const_cast` and raw pointers are outside this document's scope (they are a
memory-safety concern, handled by the normal review and the banlist).

## 4. How each need is met without a runtime

Each section names the need, how it is met today (the thing being removed),
and the shape that replaces it.

### 4.1 Reading frames off a transport

- **Was:** each library's `StdioTransport` spawned a reader job that looped
  on `getline` and fed the engine from that thread. *(gone)*
- **Rule shape:** the library has no transport that reads. The engine takes
  `step(engine, Received{line})`, with `jsonrpc::LineSplitter` (a value
  type) for unframed streams. It is called by whoever has the bytes.
- **agentty:** `rpc::Peer`'s reader task reads the channel (a child's
  stdio, our own fds, an HTTP transport presented as a pipe) and steps the
  engine under its owner (§4.4).

### 4.2 Writing frames

- **Was:** a `Transport` callback the engine called, under a write mutex.
  *(gone)*
- **Rule shape:** the engine never writes. Every operation that produces
  output **returns it**: `Outcome { std::vector<std::string> out; ... }` (or
  appends into a caller-provided sink, synchronously). Ordering is the call
  order; there is nothing to lock.
- **agentty:** `rpc::Peer` queues frames in an ordered outbox; its writer
  task drains it, so no lock is held across a write.

### 4.3 Request/response matching and timeouts

- **Was:** `request_raw` returned a `std::future`; a waiter table under a
  mutex; a deadline thread failed late waiters. *(gone)*
- **Rule shape:** the engine is an Elm model (§5):
  - `request<M>(engine, params, deadline) -> {RequestId, Effects}` records a
    pending entry; `Effects.send` holds the frame to write.
  - `step(engine, Received{line})` resolves it into `Effects.completed` as
    `{RequestId, method, expected<Json, RpcError>}`; `result<M>(c)` decodes
    it to `M::result`.
  - `step(engine, Tick{now})` fails every entry past its deadline.
    `next_deadline()` says when the next Tick is worth sending.
  - No futures. No promises. A completion is a value handed back.
- **agentty:** `rpc::Peer` parks each completion by id, and the caller
  waits for its own (`guarded::wait_with`). Its ticker sleeps to
  `next_deadline()` on `maya::delay_for` and steps `Tick{now}`.

### 4.4 Thread safety of an engine/connection object

- **Was:** `RpcEngine` and `ClientProvider` locked internally so any thread
  could call them. *(gone)*
- **Rule shape:** an engine is **single-owner**. Its methods are not
  thread-safe and say so. No member is atomic or locked.
- **agentty:** each connection's engine lives in one `maya::guarded` inside
  its `rpc::Peer`; every step happens under it, and effects are performed
  outside it.

### 4.5 Handlers, async replies, cancellation

- **Was:** handlers registered on the engine ran on the reader thread;
  async handlers captured a `Responder`; cancellation watchers spawned
  polling jobs. *(gone)*
- **Rule shape:** the peer's requests come back as **values** in
  `Effects.calls` (`Call{Id, method, params}`); the engine runs no handler.
  Replying is `reply<M>(id, result) -> frame`, written by the owner whenever
  it has the answer. Giving up on one of OUR requests is
  `step(engine, Cancel{RequestId})`; telling the peer is the protocol's own
  notification. The peer's cancel arrives in `Effects.notifications`.
  A typed **dispatch table** (`Router<Ctx>`) is a pure helper:
  `router.handle(ctx, call) -> Maybe<frame>` runs synchronously on the
  caller's thread, Nothing for a deferred method.
- **agentty:** decides which requests run inline and which go to a worker
  (`Peer::Deferred<M>` answers once, later), and owns every cancellation
  token: `CallOptions::cancel` is a real `std::stop_token`, wired to a
  `stop_callback`, never polled.

### 4.6 Data-parallel work (rag-cpp index build, batch embedding, search)

- **Today:** `rag::util::parallel_for` runs through an installed `Executor`
  (with a `std::jthread` fallback); some indexes hold a mutex.
- **Rule shape:** a function that *can* be split takes the splitter as an
  argument:

  ```cpp
  // A Splitter runs fn(i) for i in [0, n). The library never creates one.
  using Splitter = std::function<void(std::size_t n,
                                      const std::function<void(std::size_t)>& fn)>;
  Result<Index> build_index(const Corpus&, const Options&,
                            const Splitter& split = serial);
  ```

  `serial` is a plain loop and is the default. Each `fn(i)` writes only to
  its own pre-sized slot (no shared mutable state, so no lock). Index
  structures are built, then frozen; queries on a frozen index are `const`
  and read-only, which is what makes concurrent queries safe without a lock.
- **agentty:** passes a splitter built on `maya::scope`. Mutating an index
  that others query is agentty's job: build a new one and swap it with
  `maya::published`.

### 4.7 Caches and registries inside a library

- **Today:** `rag::cache`, `rag::plugin::registry`, mcp-cpp's
  `cap::registry`, `fs_helpers` snapshots, all mutex-guarded statics.
- **Rule shape:** a cache is an **object the caller owns** (`Cache c;
  c.get(key)`), not a static. A registry is a value built once and passed in
  (`Registry r = Registry::builtin(); engine.use(r);`), or an immutable table.
- **agentty:** owns the instance; shares it across threads in a
  `maya::guarded`/`maya::published` if it needs to.

### 4.8 Processes (mcp-cpp `tools/process.cpp`, stdio servers)

- **Was:** mcp-cpp spawned server processes and ran shell tools itself, with
  reader threads and output mutexes. Server processes are now agentty's
  (`mcp::stdio_link`); `ChildProcess` remains in mcp-cpp as the portable
  spawn primitive agentty calls.
- **Rule shape:** a library describes the process (`ProcessSpec {argv, env,
  cwd, stdin}`) and parses its output (`parse_tool_output(bytes) ->
  Result`). It never starts one. Where a builtin tool's whole job is
  "run a command", the tool's `execute` takes a `HostServices` with an
  `exec(ProcessSpec, limits) -> ExecResult` that agentty implements (this
  already exists for the shell tool: `HostServices::exec`; it becomes the
  only way).
- **agentty:** implements `exec` on `maya::platform::process` with its idle
  and wall clocks (src/tool/util/exec.cpp).

### 4.9 Coroutines

- **Was:** `mcp::co`/`acp::co` `Task` types with an `await_jobs` registry
  that spawned a job per awaited future. *(deleted from both)*
- **Rule shape:** coroutine *types* (a `Task<T>` promise type, awaitables
  that suspend on a library-defined event) may live in the shared core, but
  they never resume themselves on a thread the library chose. An awaitable
  stores the handle; the **caller** resumes it when the completion arrives
  (from `Outcome.completed`). No job per await.
- **agentty:** resumes handles from the engine owner when completions land.

## 5. One copy: the shared JSON-RPC core

mcp-cpp and acp-cpp are two dialects over one transport model. Each used to
carry its own copy of the same headers (runtime.hpp identical; coro.hpp 2
lines apart; core, codec, rpc, stdio drifted copies). Under R7 they became:

```
jsonrpc-cpp   (its own repo, third_party/jsonrpc-cpp; header-only, C++23,
               no deps but nlohmann_json)
  core.hpp      the algebra: Unit, Maybe, List, Sum, Newtype, StaticString, match
  codec.hpp     Codec<T>: serialization as a fold (record, sum_tagged, enum_codec)
  error.hpp     Id (the wire's), RequestId (ours), RpcError, errc
  message.hpp   Message = Call | Notification | Reply, parsed once at the edge
  method.hpp    Method<"name", Params, Result>, Note<"name", Params>
  engine.hpp    Engine + step(Engine&, Event) -> Effects  (the Elm model)
  router.hpp    Router<Ctx>: the peer's calls, dispatched by method type
  framing.hpp   LineSplitter: a byte stream to lines

mcp-cpp  links jsonrpc::jsonrpc.  Owns: MCP types, methods, capabilities,
         the Client/Server protocol state machines, builtin tool logic.
acp-cpp  links jsonrpc::jsonrpc.  Owns: ACP types, methods, session updates,
         the Agent/Client protocol state machines.
```

The engine is an Elm model. Its state is a value; its one transition is

```
Effects step(Engine&, Event)
Event   = Received{line} | Tick{now} | Cancel{RequestId} | Closed{reason}
Effects = send (frames, in order) + completed (our requests, answered)
        + calls (the peer asks)   + notifications (the peer tells)
```

and starting a request, `request<M>(engine, params, deadline)`, returns the
minted `RequestId` with its effects. The host is the Elm runtime around it:
it feeds events, performs effects and arms a timer for `next_deadline()`.

Types carry the protocol:

- **A method is declared once**, as `Method<"name", Params, Result>`. The
  typed `request<M>` takes exactly `M::params`, `result<M>` decodes exactly
  `M::result`, and `Router::on<M>` only accepts a handler
  `f(Ctx&, const M::params&) -> M::result`. Drift between caller, handler and
  codec doesn't compile.
- **Two kinds of id, two types.** `Id` is what the peer sent (number or
  string); `RequestId` is what we minted. A `Cancel` takes only a
  `RequestId`; neither converts into the other.
- **`Newtype` never decays.** No implicit conversion to its carrier; unwrap
  with `.get()`. (The implicit one let a `RequestId` pass as a wire `Id`.)
- **Parsed once.** A frame becomes a `Message` at the edge and is handled by
  a total `match` after. Malformed input is a value carrying the error reply
  it earns, not an exception.

Rules for the core:

- It is the **only** place JSON-RPC 2.0 is implemented. A dialect that needs
  different behaviour gets an option in the core, not a fork.
- Dialect libraries re-export its names into their own namespace (`mcp::`,
  `acp::`) with `using` where the vocabulary is shared, so callers don't care
  where a type lives.
- It obeys every rule in §2, and depends on nothing above it: not agentty,
  maya, jaal, mcp-cpp or acp-cpp.
- **No jaal.** jaal is a runtime and needs C++26; the core is a pure state
  machine and needs neither. The Elm shape is in the types, not borrowed.
- It lives in its own repo, `1ay1/jsonrpc-cpp`, as an agentty submodule.
  agentty adds it before mcp-cpp and acp-cpp; built alone, it fetches
  nlohmann_json and runs its own tests (without `-pthread`).
- **One standard: C++23** for jsonrpc-cpp, mcp-cpp, acp-cpp and rag-cpp
  (`std::expected`). acp-cpp moves up from C++20 when it adopts the core.

## 6. What agentty provides

All of it on maya names, in one module per library. Nothing here is a
library's concern.

| need | agentty module | built on |
|---|---|---|
| spawn an MCP/ACP peer, own its stdio | `src/mcp/connection.cpp`, `src/acp/connection.cpp` | `maya::platform::process`, poll reactor |
| read frames, feed the engine | same | one reader task per peer (`maya::pool` isolated job) posting lines to the owner |
| own the engine, serialise calls | same | `maya::guarded<Engine>` or a single-owner worker |
| request continuations, timeouts | same | `maya::delay_for` to `next_deadline()`, then `expire(now)` |
| HTTP/SSE MCP transport | `src/mcp/http_server.cpp` | agentty's http client, `util::WorkerGroup` |
| ACP agent serving (Zed etc.) | `src/acp/server.cpp` | stdio reader task + `maya::guarded<Sessions>` |
| builtin tools that run commands | `src/tool/util/exec.cpp` (`HostServices::exec`) | `maya::platform::process` |
| rag index build / batch embed | `src/rag/adapter.cpp` | `Splitter` on `maya::scope`; index swap via `maya::published` |
| caches/registries the libraries used to keep | the owning agentty module | `maya::guarded` / `maya::published` |

`src/tool/util/protocol_runtime.cpp` (today's `install_protocol_runtimes`)
is **deleted**: there is no runtime left to install.

## 7. How it is enforced

Lints in agentty's `static` ctest label, so one `ctest -L static` proves it:

- **`submodule_runtime`** — jaal's concurrency banlist over each library's
  `src/` and `include/`, with **empty** allowlists. The only way to pass is
  to contain none of R1's primitives. (Today it runs against per-library
  allowlists; those files are deleted when each library is done.)
- **`submodule_purity`** (new) — bans, in library code: clock reads
  (`steady_clock::now`, `system_clock::now`), sleeps, `std::future`/
  `promise`/`stop_source`, `set_runtime`/`set_executor`/`Runtime`/
  `Executor` declarations, process spawning (`fork`, `exec*`, `posix_spawn`,
  `CreateProcess`, `popen`), mutable `static` locals and globals (R3).
- **`no_duplication`** (new) — fails if mcp-cpp and acp-cpp contain a header
  whose code (comments and namespace names normalised away) matches another
  library's above a small threshold, or if either redefines a name the core
  exports (`RpcEngine`, `Codec`, `RpcError`, `Newtype` ...).
- **`jsonrpc_leaf`** (new) — jsonrpc-cpp includes only the standard library,
  nlohmann and itself, and is covered by `submodule_runtime` and
  `submodule_purity` like the other libraries. In agentty's own `src/`, only
  the integration modules of §6 include `<jsonrpc/...>`.
- **`layering` / `layering_maya` / `layering_jaal`** — unchanged: agentty
  never names jaal; maya never names agentty; jaal never names either.
- agentty's test build compiles each library's unit tests (and
  jsonrpc-cpp's) **single-threaded**: no `-pthread`, no
  `Threads::Threads` link. A library that needs a thread to pass its own
  tests fails to link.

A lint is only added together with the change that makes it pass, and its
allowlist (if any) can only shrink.

## 8. Migration plan and order

Each step leaves the tree compiling, agentty's binary working end to end,
and the `static` label green. Each library gets its own commits; agentty
bumps the submodule pointer after each.

1. **jsonrpc-cpp core.** *(done)* The shared headers extracted from
   mcp-cpp/acp-cpp into the `1ay1/jsonrpc-cpp` repo, rewritten to §4.3/§4.5 (Engine as a state machine, no
   futures, no transport, no runtime). Unit-test it single-threaded:
   request/response, ids of every JSON type, batches, deadlines via
   `expire(now)`, malformed frames, notifications, responses to unknown ids.
2. **acp-cpp on the core.** *(done)* Its runtime, rpc engine, stdio
   transport, coroutines and connection classes are gone; every method is a
   type in `protocol.hpp`. agentty drives both sides through `rpc::Peer`
   (`src/rpc/peer.cpp`): a reader, a writer and a deadline task on maya,
   the engine under one `maya::guarded`. Verified with
   `external_acp_backend_test` (in-memory, real subprocess, wedged agent)
   and an `agentty acp` stdio drive.
3. **mcp-cpp on the core.**
   - *(done)* The protocol: algebra and codecs from jsonrpc-cpp; every method
     a type in `protocol.hpp`; `Server` and `ClientHandlers` as answer
     tables; MRTR as `mrtr_retry()`; the scheduler takes a `Splitter`. The
     rpc engine, transports, coroutines, Runtime hook, `Client`,
     `ClientProvider`, `StdioServerProvider` and `Guarded` are deleted.
     agentty's `mcp::Connection` (`src/mcp/connection.cpp`) is the client:
     an `rpc::Peer` over a `Link` (stdio child or HTTP), cached lists, tool
     calls with MRTR and event-driven cancel. `mcp-serve` runs `Server` on a
     peer. Verified: mcp-cpp's 62 cases, `mcp_bridge_test`, `mcp_http_test`,
     `mcp_reload_race_test`, `plugin_disabled_tools_test`,
     `toolset_e2e_test`, `mcp-serve`, a hanging-tool cancel + reconnect
     probe.
   - Still to do: `cap::Registry` keeps its mutex; builtin tools still hold
     locks/atomics for caches (`fs_helpers`, `textproc`, `repomap`) — these
     become caller-owned objects.
4. **rag-cpp.** `parallel.hpp`'s executor becomes the `Splitter` parameter;
   corpus/hnsw/bm25 build-then-freeze; caches and the plugin registry become
   caller-owned. Verify: `rag adapter`, `rag shutdown interrupts warm
   promptly`, `search_docs`/`search_code` through `mcp-serve`.
5. **agentty cleanup.** Delete `protocol_runtime.cpp`, the per-library
   concurrency allowlists, and every library-side runtime mention in
   `LAYERING.md`. Turn on `submodule_purity` and `no_duplication`.
6. **Full verification.** Build and run `agentty_tests` once; a TSan build of
   the race tests and the ACP/MCP storms.

## 9. Decision checklist for any change

Before writing library code, answer these. Any "yes" means the code belongs
in agentty, not the library:

- Does it need to run while the caller is doing something else?
- Does it wait — for time, a reply, a pipe, a lock?
- Could two threads call it at once and need it to be safe?
- Does it read the clock, the environment, or spawn something?
- Does it keep state across calls that is not inside an object the caller
  owns?
- Does the same code already exist in another library?

And for agentty code that drives a library:

- Is it in that library's one integration module (§6)?
- Does it use only maya names for every thread, wait, lock and process?
- Is the engine touched by exactly one owner?

## 10. Glossary

- **Passive library** — code that only does work on the caller's thread, in
  the call, and returns. No background, no waiting, no shared mutable state.
- **Engine** — a JSON-RPC connection's state machine: pending requests,
  ids, deadlines. Fed bytes, returns frames and completions.
- **Owner** — the single place (a guarded value or one worker) allowed to
  call an engine. agentty decides it.
- **Outcome** — what a library call returns instead of doing I/O: frames to
  write, completions, inbound requests, cancellations.
- **Splitter** — a caller-supplied function that runs `fn(i)` over `[0, n)`,
  possibly in parallel. The only form of parallelism a library may accept.
- **Core** — jsonrpc-cpp, the one shared implementation of JSON-RPC 2.0 and
  the codec vocabulary.
