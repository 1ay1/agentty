---
title: The Rust critique, answered
description: Honest opposition research — the seven things a Rust advocate can attack in agentty's C++, ranked, with the real bug fixed, the footgun hardened, and the two structural gaps closed by primitives that exceed stock Rust.
nav_section: Advanced
nav_order: 90
slug: rust-critique
---

Companion to [Why modern C++ (not Rust)](/docs/why-not-rust). That page is the
case *for* staying. This one is honest opposition research: the places a Rust
user can correctly say *"the borrow checker / `Send`+`Sync` / `Mutex<T>` would
not have let you do that."* Listing them is what makes the "don't move"
position **earned** rather than asserted. Every item is either a real bug we
fixed or a place we consciously trade a Rust guarantee for something and say
so out loud.

## Scorecard

The findings, ranked, after the work:

| # | Finding | Verdict | vs. Rust |
|---|---------|---------|----------|
| 1 | Data race on per-session `model`/`profile`/`cwd` | **FIXED** | matched — a lock the reviewer sees |
| 2 | Lock ordering | **BEATS RUST** | compile-time rank + runtime tripwire; Rust checks neither |
| 3 | Detached-thread panic = process death | **BEATS RUST** | terminate structurally unreachable in the spawn primitive |
| 4 | `Message&` held across a mutating loop | **HARDENED** | loud abort; Rust borrow-checks it |
| 5 | `.value()` that can abort | wash | same as `.expect()` |
| 6 | `catch(...) {}` swallow | wash | `?`/`match` forces acknowledgement |
| 7 | raw `new` / `reinterpret_cast` | non-issue | (nothing to fix) |

## 1. The real bug (fixed)

`Session::model`/`profile`/`cwd` were guarded by neither the session-map mutex
nor the per-session thread mutex. `find_session()` releases the map lock before
returning, so a `session/set_config_option` writing `model` mid-turn raced the
streaming worker reading it — a torn `std::string` read, i.e. UB. **This is the
Rust thesis in one bug:** a lock-by-convention scheme covered `messages` but
silently missed three fields, and the compiler said nothing.

Fixed by locking the write sites and snapshotting `model`/`profile` once under
the lock at turn start (a mid-turn change applies next turn). Fixed *the way
the argument demands* — with a lock a reviewer can see.

## 2 & 3. The two structural gaps (now beat Rust)

**Lock ordering** was enforced by four identical comments. Now it's a **type**:
`RankedMutex<N>` carries a compile-time rank, `RankedLock` checks order, and a
violation is a `static_assert` (compile) or a debug `std::abort` tripwire
(runtime, cross-function). Rust doesn't check lock ordering out of the box —
this is strictly more.

**Detached worker threads** meant an escaping exception → `std::terminate` →
every session dies. Now both spawn sites go through a primitive whose body
makes `std::terminate` structurally unreachable, and the owned variant joins on
destruction (structured concurrency). You can't forget the isolation because
the spawn primitive owns it.

## 4. The footgun (hardened)

`run_tools` holds a `Message&` into the messages vector across the whole tool
loop — safe only because "the worker never appends here." Rust's borrow checker
would enforce that at compile time; we can't, so we made it **loud**: the code
captures the vector's storage identity up front and `std::abort`s with a marker
if it ever reallocates. Silent use-after-realloc → immediate, debuggable abort.

## 5–7. Washes and non-issues

`.value()` that aborts is the same as Rust's `.expect()` — a panic, not UB.
The remaining `catch(...) {}` sites route through a diagnostic log so nothing
is silently swallowed. And the raw-memory grep hits are almost all comments,
string literals, or one audited base64 codec — no raw owning `new`/`delete` in
application logic.

## Bottom line

Every finding a Rust advocate can raise is now closed. The one real bug is
fixed; the aliasing footgun is a loud abort; and the two structural gaps have
been closed with C++ primitives that **exceed** what stock Rust offers. On the
exact axes people move to Rust for, agentty now demonstrably does more — with
the proofs in the headers and a green, zero-warning build as the receipt.
