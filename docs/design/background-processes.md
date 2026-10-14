# Background processes: the design (agentty → mcp-cpp → maya → jaal)

This is the plan for `process_start` / `process_poll` / `process_stop` and the
host Session behind them. It replaces a design whose correctness rested on
call-site discipline with one where the types carry it.

## What is wrong today (verified in code)

1. **The Proc is shared mutable state with no owner.** `state::Proc` holds a
   rolling buffer, a delivery cursor and flags. It lives in `ToolState::procs`
   as a `shared_ptr`, and `run_poll`/`run_stop` take the shared_ptr out of the
   map, drop the lock, then mutate the Proc in later `with()` calls. Two calls
   on the same id interleave between those calls: `run_stop` erases the id,
   `run_poll` already holds the pointer and keeps folding into a session that
   is gone, and `take_new` hands the same bytes to whichever call gets there
   first. The lock protects each step; nothing protects the sequence.
2. **Reading a Proc field is only safe because of a convention.** Every
   `session->running` etc. is wrapped in `call.with([&](ToolState&){...})`
   just to borrow the lock that guards the map, although the Proc isn't in
   the map any more after a stop. The type system knows nothing of this.
3. **Output is buffered twice.** The host Session keeps `pending`, mcp-cpp
   keeps a 128 KiB rolling window plus `delivered`/`dropped_unseen`. Two
   cursors over one stream, kept in sync by `fold`.
4. **Destroying a Session blocks the caller for up to 2 s.** `~JaalSession`
   runs `stop_with` and then `drain_.stop()`, which joins the pump, and the
   pump waits up to 2 s for the kill to land. That destructor runs wherever
   the last `shared_ptr` dies: inside `run_start`'s reap loop, in `run_stop`,
   or at process exit. A tool call can stall on an unrelated session.
5. **`GuardedToolState` smuggles an opaque callable through `guarded::with`**
   (`FnRef` opted into Sendable). The rank check can't see what the function
   does. Any mcp-cpp body that destroys a Session inside `with()` would run a
   2 s join under the state lock.
6. **The stop request is polled.** `stop_with` sets a flag; the pump notices
   it on its next 100 ms reactor wakeup. Every stop costs up to 100 ms of
   latency for nothing.

## The design

One rule: **a background process is an actor.** One thread (the pump) owns
the child, the pipe, the output buffer and the read cursor. Everyone else
holds a handle and sends it requests. Nothing outside the pump can touch the
pump's data, so there is nothing to interleave.

### Ownership

```
mcp-cpp ToolState::procs  : id -> shared_ptr<Session>     (map only)
Session (host)            : a handle; co-owns Core
Core   (co_owned, Sync)   : guarded<Stream> + stop_source + waker
pump thread               : owns process_t + reactor; co-owns Core
```

- `Stream` (inside the guarded) is the ONLY copy of the output: a rolling
  window with an absolute cursor. `read(max)` returns what's new and moves the
  cursor in one exclusive section, so two concurrent polls on one id each get
  a disjoint slice. There is no second buffer and no `fold`.
- The pump co-owns `Core`. The Session does not join the pump. Dropping the
  last Session requests stop and returns at once. The pump kills the child,
  waits for it, reaps it and exits on its own. A pump still running at
  process exit is the pool's to reap (WorkerGroup in the background pool).
- Stop is an event, not a poll: `Core` holds the pump's reactor waker, and
  `stop()` wakes it immediately.

### Interfaces (mcp-cpp `Session`)

```cpp
struct Session {
    struct Read {
        std::string output;        // new since the last read, at most max bytes
        std::size_t dropped = 0;   // bytes that scrolled away unseen
        bool running = true;
        bool eof = false;          // pipe closed (no more output can come)
        std::optional<ExecOutcome> outcome;   // set once the child is reaped
        std::chrono::seconds uptime{0};
    };
    // Wait up to `wait` for news, then take it. Thread-safe; two readers get
    // disjoint output. Never blocks longer than `wait`.
    virtual Read read(std::chrono::milliseconds wait, std::size_t max) = 0;
    // Ask it to stop. Never blocks. Idempotent.
    virtual void stop() noexcept = 0;
    // Destruction never blocks either.
};
```

`state::Proc` becomes `{id, command, shared_ptr<Session>}`: immutable after
insert. mcp-cpp's `with()` bodies only insert, find, and erase map entries.
Everything about the process goes through the Session, which is thread-safe
by itself.

### Why it can't race

- The Session's mutable state is a `jaal::guarded<Stream>`: no lock held
  across I/O, bodies captureless, results by value, rank-checked.
- The pump is a jaal job (captureless body, Sendable args) and the shared
  part is a `co_owned<Core>` whose `Core` is `Sync` (const fields, guarded,
  atomics), so jaal's compile-time checks apply.
- mcp-cpp's ToolState holds only a map of immutable entries plus the counter;
  a `with()` body can't call into a Session, because a Session's methods are
  only reached through the shared_ptr after it leaves the map.
- No destructor waits: dropping a Session anywhere (inside a lock, at exit)
  is safe.

### Tests

- mcp-cpp `process_tools_test` (existing behaviour) on the new interface.
- New: concurrent poll+poll on one id gets disjoint output that adds up to
  the stream; concurrent poll+stop on one id; stop latency < 100 ms; dropping
  a session never blocks (< 50 ms) even when the child ignores SIGTERM; 32
  sessions started and dropped while polled.
- `exec_session_race_test` under ASan and TSan.
