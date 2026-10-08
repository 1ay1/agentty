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
5. **The other submodules contain no runtime at all.** mcp-cpp, acp-cpp,
   rag-cpp and claybin are passive libraries: no threads, pools, timers,
   locks, atomics, `std::async`, futures, `detach()`, thread-locals, clocks
   or processes — not even a standalone fallback, and no `Runtime`/
   `Executor` hook they call out through. They are state machines that agentty
   drives on maya → jaal. mcp-cpp and acp-cpp share one JSON-RPC core instead
   of keeping copies. The full rules, the replacement shape for every need,
   and the migration plan are in
   **[PROTOCOL_LIBRARIES.md](PROTOCOL_LIBRARIES.md)**, which is binding.

## How a library gets concurrency without owning it

It doesn't. A library is a state machine: it takes bytes, values and `now`
as arguments and returns frames, completions and deadlines as values.
agentty owns every reader, timer, lock and process around it and drives it
from one owner per connection. See
[PROTOCOL_LIBRARIES.md](PROTOCOL_LIBRARIES.md) §4 for the shape that
replaces each need (reading frames, request matching and timeouts, handlers,
data-parallel work, caches, processes, coroutines) and §6 for which agentty
module provides each.

The old pattern here, a `Runtime`/`Executor` interface the library calls out
through with a `std::jthread` fallback, is being removed (§8 of that doc):
it still let the library decide when concurrent work happens.

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

None, once PROTOCOL_LIBRARIES.md §8 is done. Until then
`tools::util::install_protocol_runtimes()` (src/tool/util/protocol_runtime.cpp)
installs maya-backed implementations of the libraries' remaining runtime
hooks; it is deleted in step 5 of that plan.

## Where it stands

- agentty → maya → jaal is strict: no jaal include or name in agentty.
- maya starts no thread of its own.
- claybin is fully passive. mcp-cpp, acp-cpp and rag-cpp start no threads
  of their own, but still carry a standalone fallback runtime, locks around
  their own state, and duplicated JSON-RPC code. Removing all of it is
  [PROTOCOL_LIBRARIES.md](PROTOCOL_LIBRARIES.md) §8.
