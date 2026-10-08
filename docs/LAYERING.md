# Layering: agentty → maya → jaal

This is a rule, not a description. Where the code disagrees, the code is the
thing to fix.

## The rule

```
agentty          orchestrates: owns the program, decides what runs when
   │
   ▼
 maya            the one face agentty sees: view, terminal, AND the runtime
   │             (re-exported from jaal under maya's own names)
   ▼
 jaal            the runtime: loop, effects, tasks, subscriptions, threads,
                 locks, cancellation, processes, polling
```

1. **agentty depends on maya, never on jaal.** No `#include <jaal/...>`, no
   `jaal::` name, no build reference to `third_party/maya/third_party/jaal`,
   anywhere in agentty's `src/`, `include/` or `tests/`. Everything agentty
   needs from the runtime it gets as `maya::...` from `<maya/...>`.
2. **maya depends on jaal, and jaal on nothing above it.** jaal never includes
   maya. maya's view and terminal code (`maya` core) does not include jaal
   either; only maya's runtime face does (`maya::app`, `<maya/runtime*.hpp>`,
   `<maya/host/...>`).
3. **All runtime and concurrency goes through maya → jaal.** Threads, worker
   pools, background jobs, timers, locks, atomics used for coordination,
   condition variables, thread-locals, cancellation, child processes, I/O
   polling. agentty uses maya's runtime API for these; nothing else in the
   tree starts a thread or takes a lock of its own.
4. **agentty orchestrates.** It decides what runs, on which worker, under
   which cancellation, and for how long. It hands the other libraries the
   capabilities they need; they do not go and get them.
5. **The other submodules own no runtime.** mcp-cpp, acp-cpp, rag-cpp and
   claybin are libraries, not programs. They contain no threads, pools,
   timers, locks, atomics-for-coordination, `std::async`, `detach()` or
   thread-locals. Where one needs concurrency or a process, it **declares an
   interface** and the host (agentty) **implements it on maya → jaal**.

## How a library gets concurrency without owning it

The pattern already exists in mcp-cpp, and every library follows it:

```cpp
// mcp-cpp: states what it needs, owns nothing
struct HostServices {
    std::shared_ptr<Exec> exec;      // null ⇒ tools that run programs are off
    ...
};

// agentty: provides it, built on maya's runtime
class JaalExec final : public mcp::tools::Exec { ... };   // tool/util/exec.cpp
svc.exec = std::make_shared<JaalExec>(defaults);
```

The library calls an interface. agentty implements it with maya's runtime
primitives (a worker group, a scope, a guarded value, a process and a poll
reactor) and installs it. So:

- a library's behaviour under cancellation and shutdown is agentty's policy,
  enforced in one place;
- tests can hand a library a deterministic fake (maya's `sim` host);
- there is exactly one thread pool, one shutdown path and one lock discipline
  in the process.

The same shape applies to:

| library  | needs                                   | declares                          |
|----------|-----------------------------------------|-----------------------------------|
| mcp-cpp  | running programs, RPC over stdio/HTTP,  | `HostServices` (exec, transport,  |
|          | per-request timeouts, parallel search   | executor); no threads inside      |
| acp-cpp  | the stdio RPC loop, request cancellation| an executor + transport interface |
| rag-cpp  | parallel indexing / embedding / search  | an executor for parallel loops    |
| claybin  | spawning a sandboxed child              | nothing it runs itself            |

A library may keep plain, single-threaded data structures. It may not decide
*when* or *on what thread* anything runs.

## What maya exposes for this

`<maya/runtime.hpp>` re-exports the jaal pieces an app uses, as aliases so
there is no cost and no copied code:

- **program and effects:** `maya::Cmd`, `Sub`, `Sink`, `pure_fx`, `Program`,
  `run`, `host_context`, `payload_t`, `require_host_for`
- **concurrency:** `maya::guarded`, `scope` / `nursery`, `pool`, `delay_for`,
  `stop_group`, `loop_bound`
- **thread-safety marks:** `Sendable`, `sendable_opt_in`, `frozen_opt_in`,
  `shared`
- **platform:** process, poll reactor, file lock, clock
- **test hosts:** `sim`, `headless`, `explore`

When agentty needs something jaal has and maya doesn't re-export yet, it is
added to maya first. That list is maya's runtime API, and keeping it explicit
is what keeps the layering honest.

## How it's enforced

All are static ctest entries (label `static`), no build needed:

- **layering** — fails on `<jaal/` or `jaal::` in agentty's `src/`,
  `include/` or `tests/` (tests/lint/layering.cmake).
- **submodule_runtime** — jaal's concurrency ban-list, TIGHT, over each
  library's `src/` and `include/` against the library's own
  `tests/lint/concurrency_allowlist.txt` (tests/lint/submodule_runtime.cmake).
- **concurrency_banlist_src / _include** — the same ban-list over agentty.
  The allowlist names the few files that ARE the implementation of a safe
  type and says why.
- **elm_purity** — reducers read only the Model and return effects.
- maya's own `tests/seam.sh` — only `maya/host/` and `maya/runtime.hpp` may
  depend on jaal, and maya starts no thread of its own.

## The runtimes agentty installs

`tools::util::install_protocol_runtimes()` (src/tool/util/protocol_runtime.cpp)
runs right after argument parsing, before any subcommand:

| library | interface             | agentty's implementation                      |
|---------|-----------------------|-----------------------------------------------|
| mcp-cpp | `mcp::Runtime`        | job = isolated task on a `maya::pool`;        |
|         | `tools::Executor`     | `parallel_for` = `maya::scope`;               |
| acp-cpp | `acp::Runtime`        | `sleep_for` = `maya::delay_for`               |
| rag-cpp | `rag::util::Executor` | `maya::scope` (nests without deadlock)        |
| maya    | `maya::exec`          | installed by `maya::run` on jaal              |

Each library also ships a small standalone fallback (plain `std::jthread`,
joined) for programs that install nothing. It is the one place that library
names a thread, and agentty replaces it at startup.

## Where it stands

- agentty → maya → jaal is strict: no jaal include or name in agentty.
- maya starts no thread of its own.
- mcp-cpp, acp-cpp, rag-cpp and claybin start no threads of their own. What
  remains on their allowlists is the standalone fallback and locks/atomics
  that protect a structure's own state (an engine's waiter table, a cache, a
  work cursor) — they make a type safe to call from the host's threads and
  run nothing.
