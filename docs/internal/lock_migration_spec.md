# Lock-order migration spec (agentty on jaal typed-locks)

You are converting C++ code in C:\Develop\agentty to jaal's COMPILE-TIME lock
order. Read this whole file before editing anything.

## The API (already implemented; do NOT change jaal/maya)

Headers: third_party/maya/third_party/jaal/include/jaal/core/capability.hpp,
jaal/kernel/guarded.hpp, jaal/kernel/snapshot.hpp; re-exported in
third_party/maya/include/maya/runtime.hpp as maya::*.

- `maya::guarded<T, Rank = maya::leaf>`. Ranks are types, defined ONLY in
  include/agentty/util/lock_levels.hpp (namespace agentty::lock_levels,
  alias `agentty::locks`): mcp_connecting(10) < copilot_auto(12) <
  auth_refresh(15) < rag_index(20) < memory_store(25) < file_lane(30) <
  worker_slot(35) < writer_state(40) = settings_cache(40) < maya::leaf.
  Parameter aliases: locks::below_auth_refresh, locks::below_file_lane, etc.
- Capabilities (non-copyable, non-movable, pass by `const&`):
  - `const maya::unlocked&` — thread holds no lock.
  - `const maya::held<R>&` — inside a lock body of rank R.
  - `const maya::below<R>&` — PARAMETER type: caller holds only ranks < R.
    `unlocked` and `held<Q>` with Q<R convert to it implicitly.
- Every access takes a capability FIRST:
  `g.with(cap, body, args...)`, `g.read(cap, ...)`, `g.try_with(cap, ...)`,
  `g.try_read(cap, ...)`, `g.wait_with(unlocked_cap, pred, body, args...)`,
  `g.wait_with_for(unlocked_cap, dur, pred, body, args...)`.
  A body may take `const maya::held<R>& h` as its FIRST parameter if it needs
  to take a further (higher-ranked) lock; otherwise the old body signature
  (T&, args...) still works unchanged.
- Blocking ops need exactly `const maya::unlocked&`: wait_with*,
  `maya::scope(cap, block)` / `maya::scope(cap, stop_token, block)`,
  `pool.shutdown(cap[, grace])`, `maya::worker_group::stop(cap)`,
  `util::WorkerGroup::stop(cap)`, `util::teardown::run(cap)`.
- Thread entry points that may receive a fresh `const maya::unlocked&`
  (just add it as the first parameter of the body):
  - util::run_background / run_isolated_detached / WorkerGroup::post bodies:
    `[](const maya::unlocked& cap, std::stop_token st, Args...)`
  - maya::pool post/submit/post_isolated/submit_isolated bodies: same form.
  - Cmd::task / task_isolated / Sub::stream bodies:
    `[](const maya::unlocked& cap, maya::Sink<Msg> out, std::stop_token st, Args...)`
  - cmd::io_task / io_task_isolated / io_stream bodies:
    `[](Io io, const maya::unlocked& cap, maya::Sink<Msg> out, std::stop_token st, Args...)`
  - maya::scope nursery.spawn bodies: `[&](const maya::unlocked& hc)` or
    `[&](const maya::unlocked& hc, std::stop_token)`.
  - util::teardown::on_shutdown actions: `[](const maya::unlocked& cap) {...}`.
- `maya::snapshot<T>` (T must be Shareable: Sendable+Frozen — plain structs of
  strings/numbers/std containers; NOT nlohmann::json, NOT std::function):
  lock-free read-mostly value. `s.load()` -> shared handle (deref with * or
  ->), `s.get()` -> copy, `s.store(v)`, `s.update(f, args...)` where f is a
  CAPTURELESS `T(const T& old, const Args&...)` (may run more than once).
  No capability needed anywhere.
- `maya::published<T>` (std::shared_ptr slot): `publish(sp)`, `current()`,
  `take()`, `replace(sp)`. No capability needed. Use for owners of things that
  can't be in a guarded (worker groups, pools, std::function objects).
- `maya::thread_root([&](const maya::unlocked& cap) { ... })` mints a root
  capability. It is a REVIEWED escape hatch: allowed ONLY at a genuine thread
  root where you can argue no lock is held: main(), a raw std::thread entry,
  a third-party library callback on its own thread, a destructor/virtual
  override provably never run under a lock, or an "effect seam" called only
  from the effect host. Every use MUST carry a comment stating WHY no lock is
  held there. Prefer threading a real capability parameter instead.
  NEVER use thread_root inside a function that might be called from a lock
  body — that defeats the whole system.
- guarded<T> static_asserts JoinFree<T>: T may NOT own (directly, in a
  container, optional, unique_ptr, shared_ptr) a maya::pool,
  maya::worker_group, util::WorkerGroup. Move such owners out of the lock into
  a `maya::published<X>` slot (publish/take) and stop them with `unlocked`.
- `maya::lock_level`, `lock_levels::kXxx` constants, `lock_order_error` for
  nesting, and `forget_held_after_fork` NO LONGER EXIST.

## Design rules (follow them; this is the point of the work)

1. Decide each lock's shape first:
   - Value replaced wholesale, read often (caches of a value, "current X",
     flags, published config)? -> `maya::snapshot<T>` (or an atomic for a
     single bool/int). No rank, no capabilities. PREFER THIS where it fits.
   - Genuine critical section (read-modify-write over a structure, waiters)?
     -> `maya::guarded<T>` (leaf by default).
   - Its body must take ANOTHER guarded? -> it needs a named rank BELOW the
     inner one. Only use ranks from lock_levels.hpp; if you need a new rank,
     STOP and report it instead of inventing one (the coordinator owns that
     file).
2. Public functions that (transitively) take a lock get a capability
   parameter, placed right after any `Io` parameter (or first if none):
   - only leaf locks reached -> `const maya::below<maya::leaf>& cap`
   - reaches rank R or higher -> `const locks::below_R& cap`
   - blocks/joins/waits -> `const maya::unlocked& cap`
   Thread that capability down; do not mint a new one.
3. Never hold a lock across network IO or a blocking wait. If the old code
   did, restructure (copy out, do IO, copy in) unless it's an intentional
   single-flight lane with a proper rank (like auth_refresh).
4. Keep behavior identical otherwise. Preserve comments; update ones that
   describe the old runtime lock_order checker.
5. Destructors and library-called virtual overrides can't take a capability.
   If one must lock, prefer making that state lock-free (atomic/snapshot).
6. Do not touch files outside your assigned list, EXCEPT to update callers of
   a public function whose signature you changed — and then only the call
   expression (pass the capability you have in scope). Report every such
   cross-file call site you changed or could not resolve.

## Settled boundary contracts (the coordinator decided these; implement them)

Already changed in headers (do not change again):
- include/agentty/provider/provider.hpp: `concept Provider` is
  `p.stream(const maya::unlocked&, Request, EventSink) -> StreamResult`.
  Every provider class's `stream` gains `const maya::unlocked& cap` FIRST.
- include/agentty/provider/dispatch.hpp: `StreamFn` =
  `std::function<StreamResult(const maya::unlocked&, Request, EventSink)>`;
  `ProviderRouter::external_acp` gains `const maya::unlocked&` first;
  `dispatch_stream(const maya::unlocked&, routes, [sel,] req, sink)`.
- include/agentty/provider/acp_backend.hpp: `AcpBackend::prompt(const
  maya::unlocked&, const Request&, const TurnSink&, const CancelTokenPtr&)`.
- include/agentty/runtime/app/deps.hpp: `Deps::stream` is
  `std::function<void(const maya::unlocked&, Request, EventSink)>`. The turn
  worker in cmd_factory.cpp (an io_task body) takes
  `(Io io, const maya::unlocked& cap, maya::Sink<Msg> out, std::stop_token, ...)`
  and passes `cap`.
- include/agentty/tool/registry.hpp: `ToolDef::execute` is
  `ToolDef::ExecuteFn = std::function<ExecResult(const maya::unlocked&,
  const nlohmann::json& args, const CallContext& ctx)>`. Every `def.execute =
  [..](const json& args, const CallContext& ctx)` lambda gains
  `const maya::unlocked& cap` FIRST.
  include/agentty/tool/util/tool_args.hpp `adapt(parse, run)` accepts
  `run(const Args&)` (unchanged tools) OR `run(const maya::unlocked&, const
  Args&)` (tools that block/lock).
- include/agentty/tool/tool.hpp: `DynamicDispatch::execute_with(cap, td,
  name, args, ctx)`, `execute(cap, name, args, ctx)`,
  `execute_approved(cap, name, args, hash, ctx)`.
- include/agentty/rpc/peer.hpp (+ src/rpc/peer.cpp, compiles): Channel
  read/write/interrupt take `const maya::unlocked&` first; Dispatch
  on_call/on_notification take `const maya::unlocked&` first (reader
  thread); `Peer::call<M>(const maya::unlocked&, params, opts)`, `call_raw(cap,
  ...)`; notify/notify_raw/reply/Deferred::ok/error/set_request_meta/closed
  take `const maya::below<maya::leaf>&` first; `cancel(const maya::unlocked&,
  id)`; `stop(const maya::unlocked&)`, `wait_closed(const maya::unlocked&)`;
  the RAII `Peer::Batch` is GONE -> `peer.batch(cap, [&]{ ... })`;
  `CallOptions::on_cancel` is `std::function<void(const maya::unlocked&)>`
  (runs on the calling thread). `~Peer()` stops and joins by itself (Peer is
  joins_on_destroy, so it can never be owned by a guarded): an owner may
  simply destroy/reset its Peer in a destructor.
- Library callbacks that run on their OWN thread with nothing held (a
  third-party virtual like mcp-cpp `CapabilityProvider::execute` called
  from mcp's server loop, a jsonrpc::Router handler invoked from a
  Dispatch callback) may receive their capability by threading it through a
  context struct you control (preferred), or, where the signature is fixed
  by a third-party library, by a `maya::thread_root` at that ONE entry with a
  comment naming the thread and why it holds nothing. List each such root in
  your report.
- A destructor that must stop something: prefer that the owned object's own
  destructor does it (Peer, pool, worker_group all stop+join themselves),
  i.e. just let it be destroyed / `reset()` it.

## Verify

FAST CHECK (use this, seconds per file, runs files in parallel):
  powershell -NoProfile -File scripts\check_tu.ps1 -Files "src/a.cpp,src/b.cpp" -Max 20
run from C:\Develop\agentty. It syntax-checks each translation unit with the
real flags and prints only that file's errors (errors inside headers it
includes show too). A header you change is checked through the .cpp files
that include it. Iterate until every file in your scope prints OK.

Do NOT run the full cmake build: other subsystems are being converted in
parallel by other agents, and the coordinator runs the full build at the
end. Report: files changed, public signatures changed (old -> new),
cross-file call sites touched or needing others' attention (file:line +
which capability is in scope there), any lock you couldn't rank or any
design question, and every thread_root you added with its justification.
