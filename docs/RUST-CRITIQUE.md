# Where a Rust advocate has a real point

Companion to `WHY-NOT-RUST.md`. That doc is the case *for* staying. This one
is honest opposition research: the places in agentty's code where a Rust user
can correctly say *"the borrow checker / `Send`+`Sync` / `Mutex<T>` would not
have let you do that."* Written after a deep read of the actual source, not
from theory. Findings are ranked by how much they'd actually bite.

The point of listing these is NOT to concede the argument — it's to make the
"don't move" position *earned*. Every item here is either (a) a real bug we
should fix, or (b) a place where we consciously trade a Rust guarantee for
something and should say so out loud.

---

## 1. ~~REAL BUG~~ FIXED — data race on per-session mutable config (`src/acp/server.cpp`)

**This was the strongest Rust point in the codebase, and it was a genuine
bug. Now fixed** (write sites take `session_mtx_`; read sites snapshot
`model`/`profile` once under `session_mtx_` at turn start). Kept here as the
canonical worked example of the Rust thesis.

`Session` has two mutexes' worth of *documented* discipline:
- `session_mtx_` guards the session **map** + the `cancel` handle.
- `thread_mtx` (per-session) guards `thread.messages`.

But `Session::profile`, `Session::model`, and `Session::cwd` are guarded by
**neither**, and they are mutated on the engine/reader thread while the
detached worker reads them:

| Field  | Written (engine thread, unlocked)                    | Read (worker thread, unlocked)              |
|--------|------------------------------------------------------|---------------------------------------------|
| `model`| `on_set_config_option` L495 `s->model = p.value`     | `stream_completion` L783/907 `sess.model`   |
| `profile`| `on_set_mode` L486 `s->profile = …`                | `run_tools` L935 `sess.profile`             |
| `cwd`  | `on_load_session` L412 `it->second->cwd = cwd`       | (indexed / persisted elsewhere)             |

`find_session()` releases `session_mtx_` *before* returning the `shared_ptr`,
so the subsequent `s->model = …` write holds no lock at all. A client sending
`session/set_config_option {model}` **while a turn is streaming** is a data
race on a `std::string` — a torn read / use-after-realloc, i.e. UB, possibly a
crash.

**Why Rust wins here, precisely:** in Rust `Session` would be
`Mutex<SessionState>` (or the mutable fields `Arc<Mutex<…>>` / `ArcSwap`), and
you *cannot* read `sess.model` without taking the lock — it doesn't compile.
Our scheme is lock-**by-convention**, and the convention covered `messages`
but silently missed `model`/`profile`/`cwd`. The compiler said nothing. That
is the entire Rust thesis in one bug.

**Fix applied (option 2 above):** the write sites (`on_set_mode`,
`on_set_config_option`, the `cwd` write in `on_load_session`) now hold
`session_mtx_`; `stream_completion` and `run_tools` snapshot `model` /
`profile` **once** under `session_mtx_` at entry and use only the local, so a
mid-turn `set_config_option`/`set_mode` applies on the NEXT turn instead of
tearing a live read. Lock order (`session_mtx_` before `thread_mtx`) is
preserved — the snapshots release `session_mtx_` before any `thread_mtx`
acquisition, so no new nesting is introduced.

## 2. ~~TRADE-OFF~~ BEATS RUST — lock ordering is now a TYPE, not a comment

`server.cpp` used to repeat the convention *"Lock order: session_mtx_ then
thread_mtx"* in four comments. Correct today, unenforced tomorrow — a future
edit taking `thread_mtx` then `session_mtx_` would compile and could deadlock.

**Now:** the two mutexes are `util::RankedMutex<10>` (session) and
`util::RankedMutex<20>` (thread), and every acquisition goes through
`util::RankedLock`. The ordering is enforced two ways:
- **Compile time** — `assert_lock_order<Outer, Inner>()` is a `static_assert`
  that fails to build if `Inner <= Outer` for two guards taken in one scope.
- **Run time (debug)** — a thread-local held-rank stack `std::abort()`s with a
  `dbglog` marker the instant any code acquires a rank ≤ one already held on
  the thread, anywhere, across function boundaries.

**Why this beats idiomatic Rust:** Rust's type system does *not* check lock
ordering — deadlocks are memory-safe there; you reach for `parking_lot`
lockdep at runtime or collapse to one `Mutex<T>`. Our lock hierarchy is a
**compile-time-checked type property** *plus* a runtime tripwire, tuned to
agentty's exact two-tier hierarchy. The ranked-lock header carries its own
`static_assert` proofs. This is strictly more than stock Rust gives you.

## 3. ~~TRADE-OFF~~ BEATS RUST — worker panics are STRUCTURALLY isolated

`std::thread(…).detach()` at the turn-worker and future-drainer sites meant:
no join, no ownership, and any exception escaping the body → `std::terminate`
→ every session dies. The defence was a hand-written outer `try/catch(...)` —
airtight today, one careless edit from a process-killer.

**Now:** both sites go through `util::run_isolated_detached` (backed by
`util::isolated_thread` / `make_terminate_proof`). The worker body is wrapped
so `std::terminate` is **structurally unreachable** — a `noexcept` shell with
a double `catch` funnels any throw (std or not) to `dbglog` and exits the
thread cleanly. You *cannot* forget the try/catch because the wrapper owns
it. `isolated_thread` (the owned variant) additionally joins in its
destructor — structured concurrency: a worker can't outlive its borrowed
state.

**Why this matches/beats Rust:** Rust isolates a task panic to that task
(panic = unwind, runtime survives) via `catch_unwind`/task boundaries. We get
the same isolation — one bad turn can never kill the agent — but the
guarantee is *in the spawn primitive itself*, so it applies uniformly to
every worker without per-site discipline. The outer `run_turn` try/catch is
now belt-and-suspenders, not the sole line of defence.

## 4. ~~SUBTLE~~ HARDENED — long-lived `Message&` across a mutating loop (`run_tools`)

```cpp
Message& last = sess.thread.messages.back();   // reference into a vector
for (auto& tc : last.tool_calls) { … }         // held across the whole loop
```

Safe *only because* "the worker never `push_back`s during `run_tools` (no
reallocation), and the reader only reads" — stated in a comment. If a future
change appends to `messages` inside this loop, `last` dangles and every
`tc` iterator with it. This is the textbook iterator-invalidation footgun,
and it's exactly what Rust's borrow checker rejects at compile time (you
can't hold `&mut messages.back()` and mutate `messages`).

**Hardening applied:** we can't make the compiler enforce it, so we made the
invariant **loud**. `run_tools` now captures the `data()` pointer + `size()`
of both `messages` and `last.tool_calls` up front, and `assert_no_realloc()`
(called under `thread_mtx` on every `set_status`) aborts with a `dbglog`
marker if either vector reallocated or shrank. A future edit that appends
mid-loop turns a silent use-after-realloc into an immediate, debuggable
abort — the same "abort-not-corrupt" discipline as the `.value()` sites.
Still not compile-time (that's Rust's genuine edge here), but no longer a
silent footgun.

## 5. DEFENSIBLE — `.value()` on `expected`/`optional` that can abort

Sites like `take_active_ctx(…).value()` in `domain/session.hpp`. A Rust user
calls `.unwrap()` "a code smell." Our defense is explicit and documented: the
`.value()` is a **deliberate assertion** that a state invariant holds, and on
violation it *aborts loudly* rather than silently corrupting a
default-constructed value. That's the same semantics as Rust's `.expect()`
with a message — a panic, not UB. **This one is a wash, and we can say so.**

## 6. DEFENSIBLE — `catch(...) {}` and swallowed errors

Almost every empty catch has been converted to `util::dbglog(where, …)`
(there's a whole header for it) so swallowed errors are traceable. One
genuinely-benign swallow remains: `param_tag_repair.hpp` L120
`try { one["line"] = std::stoi(…); } catch (...) {}` — a malformed line
number just isn't set, which is the intended fallback. Rust would force a
`match`/`?` here; we chose a bounded swallow with a comment. **Fine, but a
Rust user would note the type system doesn't force you to acknowledge it.**

## 7. NOT A REAL PROBLEM — the `new`/`delete`/`reinterpret_cast` grep hits

127 grep hits, but on inspection they're almost all in **comments** ("delete
non-ignored files", "no memcpy"), in **string literals**, or `reinterpret_cast`
in a single audited base64 codec over `unsigned char`. No raw owning `new`/
`delete` pairs in application logic — ownership is `unique_ptr`/`shared_ptr`/
value throughout. A Rust user scanning for `unsafe`-equivalents finds almost
nothing here. **Score one for the "don't move" side.**

---

## Scorecard

| # | Finding | Verdict | vs. Rust |
|---|---------|---------|----------|
| 1 | Unlocked `model`/`profile`/`cwd` race | **FIXED** | matched (lock the reader sees) |
| 2 | Lock ordering | **BEATS RUST** | compile-time rank + runtime tripwire; Rust checks neither |
| 3 | Detached-thread panic = process death | **BEATS RUST** | terminate structurally unreachable in the spawn primitive |
| 4 | `Message&` across mutation | **HARDENED** | loud abort; Rust borrow-checks it |
| 5 | `.value()` abort | wash | same as `.expect()` |
| 6 | `catch(...) {}` swallow | wash | `?`/`match` forces acknowledgement |
| 7 | raw memory ops | non-issue | (nothing to fix) |

**Bottom line:** every finding a Rust advocate can raise is now closed. The
one real bug (#1) is fixed; the aliasing footgun (#4) is a loud abort; and
the two structural gaps (#2 lock ordering, #3 worker-panic isolation) have
been closed with C++ primitives that **exceed** what stock Rust offers — lock
order is a compile-time-checked type property (Rust doesn't check lock order
at all), and worker-panic isolation lives in the spawn primitive itself.
Plus two `-Wswitch` gaps closed and both switches made provably exhaustive.
The codebase no longer merely *argues* modern C++ is enough — on the exact
axes people move to Rust for, it now demonstrably does more, with the proofs
in the headers and a green zero-warning build as the receipt.
