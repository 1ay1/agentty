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

## 2. TRADE-OFF — lock ordering is a comment, not a type

`server.cpp` repeats the convention *"Lock order: session_mtx_ then
thread_mtx"* in four comments. It's correct today. But nothing **enforces**
it — a future edit that takes `thread_mtx` then `session_mtx_` compiles fine
and can deadlock in production. Rust doesn't fix lock ordering for free
either (deadlocks are safe in Rust's model), but the ecosystem has
`parking_lot`/lockdep-style tooling and the idiom of a single `Mutex<Struct>`
that sidesteps two-lock ordering entirely. **We rely on reviewer vigilance;
that's a real, if minor, point.**

Mitigation without Rust: collapse the two locks where possible, or add a
debug-only lock-order checker.

## 3. TRADE-OFF — fire-and-forget detached threads, no structured concurrency

`std::thread(…).detach()` at L646 (the turn worker) and L1052 (the
future-drainer). Detach means: no join, no ownership, no cancellation handle
beyond the `CancelToken`, and an exception that escapes ⇒ `std::terminate()`
kills **every** session. The code *knows* this — `run_turn`'s body is a giant
`try/catch(...)` precisely because an escape would terminate the process.

Rust's answer (tokio tasks / `JoinHandle` / scoped threads / `?` propagation)
makes this structurally safer: a panic unwinds one task, not the runtime. Our
safety here rests entirely on that hand-written outer catch being airtight.
**It currently is — but it's defended by discipline, not structure.**

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

| # | Finding | Verdict | Rust would have... |
|---|---------|---------|--------------------|
| 1 | Unlocked `model`/`profile`/`cwd` race | **FIXED** | refused to compile the unlocked read |
| 2 | Lock ordering by comment | trade-off | idiom (single `Mutex<T>`) sidesteps it |
| 3 | Detached threads | trade-off | structured tasks isolate panics |
| 4 | `Message&` across mutation | **HARDENED** | borrow-checked at compile time |
| 5 | `.value()` abort | wash | same as `.expect()` |
| 6 | `catch(...) {}` swallow | wash | `?`/`match` forces acknowledgement |
| 7 | raw memory ops | non-issue | (nothing to fix) |

**Bottom line:** the one real bug (#1) is **fixed**; the latent footgun (#4)
is **hardened** into a loud abort; two honest structural trade-offs remain
(#2, #3), and the rest are washes or non-issues. Two `-Wswitch` gaps found
along the way (`RepoMap` in `stream_args.hpp`, `SearchCode` in `server.cpp` —
both literal "sum type gained an arm, a switch didn't follow" bugs) are
closed, and both switches are now provably exhaustive so the next new tool
Kind re-triggers the warning. That's a strong position for "don't move" — and
now it's earned: the codebase no longer ships the one bug that proved the
borrow checker's point. The Rust advocate's best shot landed, we took it
seriously, and we closed it in C++ with a lock the reviewer can see — which
is exactly the discipline the `WHY-NOT-RUST.md` argument rests on.
