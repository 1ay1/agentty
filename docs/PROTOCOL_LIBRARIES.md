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

- **Today:** `StdioTransport` spawns a reader job that loops on `getline`
  and calls `engine.feed_line` from that thread.
- **Rule shape:** the library has no transport that reads. The engine
  exposes `feed_line(bytes) -> Outcome` (and `feed_bytes` for unframed
  streams with a `LineSplitter` value type that buffers partial lines). It is
  called by whoever has the bytes.
- **agentty:** owns the child process / socket / stdio, reads it on a maya
  worker (or the poll reactor), and calls `feed_line` on the engine's owner
  (§4.4).

### 4.2 Writing frames

- **Today:** a `Transport` callback the engine calls, guarded by a write
  mutex in the transport.
- **Rule shape:** the engine never writes. Every operation that produces
  output **returns it**: `Outcome { std::vector<std::string> out; ... }` (or
  appends into a caller-provided sink, synchronously). Ordering is the call
  order; there is nothing to lock.
- **agentty:** writes `out` to the pipe/socket, in order, from the engine's
  owner.

### 4.3 Request/response matching and timeouts

- **Today:** `request_raw` returns a `std::future`; a waiter table under a
  mutex; a deadline thread on a condition variable fails late waiters.
- **Rule shape:** the engine is a state machine:
  - `send_request(method, params, deadline) -> {RequestId, frame}` records a
    pending entry and returns the frame to write.
  - `feed_line` resolves a pending entry and returns it in
    `Outcome.completed` as `{RequestId, expected<Json, RpcError>}`.
  - `expire(now) -> std::vector<Completed>` fails every entry past its
    deadline. `next_deadline() -> optional<time_point>` says when that is next
    worth calling.
  - No futures. No promises. A completion is a value handed back to the
    caller.
- **agentty:** keeps the continuation for each `RequestId` (a callback, a
  maya promise, or a coroutine handle — agentty's choice), arms a
  `maya::delay_for` to `next_deadline()`, and calls `expire(now)` when it
  fires.

### 4.4 Thread safety of an engine/connection object

- **Today:** `RpcEngine` and `ClientProvider` lock internally so any thread
  may call them.
- **Rule shape:** an engine is **single-owner**. Its methods are not
  thread-safe and say so. No member is atomic or locked.
- **agentty:** gives each connection one owner: a `maya::guarded<Engine>`
  for short calls, or a single maya worker that owns it and receives work as
  messages (the "owner you send messages to" jaal recommends). Reader
  threads never touch the engine directly; they post the line to the owner.

### 4.5 Handlers, async replies, cancellation

- **Today:** handlers registered on the engine run on the reader thread;
  async handlers capture a `Responder` and reply later "from any thread";
  cancellation watchers spawn polling jobs.
- **Rule shape:** inbound requests come back as **values** in
  `Outcome.requests` (`{RpcId, method, params}`); the library does not run
  handlers. Replying is `engine.respond(id, result) -> frame` (or
  `respond_error`), called by the owner whenever it has the answer.
  Cancellation is a frame like any other (`engine.cancel(id) -> frame`) or an
  incoming `$/cancel_request` surfaced in `Outcome.cancelled`.
  A typed **dispatch table** (`Router<Ctx>`) is still allowed as a pure helper:
  `router.handle(ctx, request) -> optional<Json>` runs synchronously on the
  caller's thread.
- **agentty:** decides which requests run inline and which go to a worker,
  holds the `RpcId` until done, and owns any cancellation token (a real
  `std::stop_token` from maya, never a library flag).

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

- **Today:** mcp-cpp spawns server processes and runs shell tools itself,
  with reader threads and output mutexes.
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

- **Today:** `mcp::co`/`acp::co` `Task` types with an `await_jobs` registry
  that spawns a job per awaited future.
- **Rule shape:** coroutine *types* (a `Task<T>` promise type, awaitables
  that suspend on a library-defined event) may live in the shared core, but
  they never resume themselves on a thread the library chose. An awaitable
  stores the handle; the **caller** resumes it when the completion arrives
  (from `Outcome.completed`). No job per await.
- **agentty:** resumes handles from the engine owner when completions land.

## 5. One copy: the shared JSON-RPC core

mcp-cpp and acp-cpp are two dialects over one transport model. Today each
carries its own copy of the same headers (runtime.hpp identical;
coro.hpp 2 lines apart; core, codec, rpc, stdio drifted copies). Under R7
they become:

```
agentty/include/jsonrpc/   (header-only, C++20, no deps but nlohmann_json)
  core.hpp        Maybe / List / Sum / Newtype / Unit, match()
  codec.hpp       Codec<T>, the codec algebra, to_json / from_json
  error.hpp       RpcError, errc
  ids.hpp         RpcId
  engine.hpp      Engine (the state machine in §4.3/§4.5), Outcome, Completed
  framing.hpp     LineSplitter (bytes → lines), frame encoding
  router.hpp      Router<Ctx>: typed method table, pure dispatch
  coro.hpp        Task<T>, awaitables resumed by the caller (§4.9)

mcp-cpp  includes <jsonrpc/...>.  Owns: MCP types, methods, capabilities,
         the Client/Server protocol state machines, builtin tool logic.
acp-cpp  includes <jsonrpc/...>.  Owns: ACP types, methods, session updates,
         the Agent/Client protocol state machines.
```

Rules for the core:

- It is the **only** place JSON-RPC 2.0 is implemented. A dialect that
  needs different behaviour gets an option or a hook in the core, not a
  fork.
- Dialect libraries put their names in their own namespace (`mcp::`,
  `acp::`) via `using` aliases where the vocabulary is shared, so callers
  are unaffected by where a type lives.
- It obeys every rule in §2. It is a library too.
- **It lives in agentty, at `include/jsonrpc/`**, its own top-level header
  directory and namespace (`jsonrpc::`), not under `include/agentty/`. No
  separate repo, no submodule.
- **It is self-contained.** A `jsonrpc/` header includes only the standard
  library, `<nlohmann/json.hpp>` and other `jsonrpc/` headers. Never
  `agentty/`, `maya/`, `jaal/`, `mcp/`, `acp/`. That is what lets the
  libraries use it without depending on agentty: `jsonrpc/` is a leaf that
  happens to be stored in agentty's tree.
- **It is the only thing from agentty the libraries may include.**
  mcp-cpp and acp-cpp may `#include <jsonrpc/...>` and nothing else from
  agentty. The parent build exports it as the INTERFACE target
  `jsonrpc::jsonrpc` (include dir `${agentty}/include`, restricted by lint to
  `jsonrpc/`), defined before the libraries are added, and they link it.
- **The libraries are no longer standalone builds.** They are built as part
  of agentty. Their own unit tests are registered by agentty's test build
  (still single-threaded, still without `-pthread`), not by a separate
  top-level CMake project.

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
- **`jsonrpc_leaf`** (new) — `include/jsonrpc/` includes only the standard
  library, nlohmann and itself; mcp-cpp and acp-cpp include nothing from
  agentty except `<jsonrpc/...>`; nothing in agentty's own `src/` needs
  `jsonrpc/` except the integration modules of §6.
- **`layering` / `layering_maya` / `layering_jaal`** — unchanged: agentty
  never names jaal; maya never names agentty; jaal never names either.
- agentty's test build compiles each library's unit tests (and
  `include/jsonrpc/`'s) **single-threaded**: no `-pthread`, no
  `Threads::Threads` link. A library that needs a thread to pass its own
  tests fails to link.

A lint is only added together with the change that makes it pass, and its
allowlist (if any) can only shrink.

## 8. Migration plan and order

Each step leaves the tree compiling, agentty's binary working end to end,
and the `static` label green. Each library gets its own commits; agentty
bumps the submodule pointer after each.

1. **jsonrpc core.** Extract the shared headers from mcp-cpp/acp-cpp into
   `agentty/include/jsonrpc/`, rewritten to §4.3/§4.5 (Engine as a state machine, no
   futures, no transport, no runtime). Unit-test it single-threaded:
   request/response, ids of every JSON type, batches, deadlines via
   `expire(now)`, malformed frames, notifications, responses to unknown ids.
2. **acp-cpp on the core.** Smaller of the two; proves the shape. Delete its
   `runtime.hpp`, `rpc.hpp`, `stdio.hpp`, `coro.hpp`, and its copies of
   core/codec. Its Agent/Client become protocol state machines over the core
   Engine. agentty's `acp/server.cpp` and `external_acp_backend.cpp` gain the
   reader task and owner. Verify: `external_acp_backend_test`, the ACP
   JSON-RPC drive (tool turn, load, list, delete) and the 3-session storm.
3. **mcp-cpp on the core.** Same for the protocol side; then
   `cap::ClientProvider`, `cap::registry`, `Guarded`, `scheduler`,
   `stdio_server` lose their locks and jobs (single-owner, agentty drives).
   Builtin tools: `process.cpp` goes through `HostServices::exec` only;
   `fs_helpers`/`textproc`/`repomap` caches become caller-owned objects.
   Verify: `mcp_bridge_test`, `mcp_http_test`, `mcp_reload_race`,
   `plugin_disabled_tools_test`, `toolset_e2e_test`, `mcp-serve` tools/list +
   calls, a real plugin round trip.
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
- **Core** — `include/jsonrpc/` in agentty, the one shared implementation of JSON-RPC 2.0 and
  the codec vocabulary.
