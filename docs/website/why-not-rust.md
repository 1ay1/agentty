---
title: Why modern C++ (not Rust)
description: agentty stays modern C++ and gets the guarantees people move to Rust for — at compile time, in the header. 155+ static_assert proofs, an enforced lock hierarchy, and structurally-isolated worker panics. The code is the argument.
nav_section: Advanced
nav_order: 80
slug: why-not-rust
---

This is not a language flame-war. It's a claim with receipts: **agentty
already gets the guarantees people move to Rust for — and it gets them at
*compile time*, in the header, next to the code they constrain.** The
codebase *is* the argument.

fish moved C++ → Rust in 4.0. Good decision for fish. This page walks through
those reasons and shows why they don't apply here — and where agentty's
approach is sharper.

## The receipts, up front

- **155+ `static_assert` / `consteval` proofs** across the headers in
  `include/agentty/`, all evaluated every build. The build is the test
  runner: a green `cmake --build` means every invariant below holds. There is
  no separate CI step that can be skipped, no fixture that can drift, no
  `#[test]` someone forgot to run.
- The proofs live **next to the code they constrain**, so a reader sees the
  contract before the implementation.

## fish's reasons, mapped onto agentty

**"The C++ toolchain was painful on old / LTS systems."** Real for a shell
that ships in every distro's base image. agentty is an application built from
a pinned toolchain — one `cmake --build`. Not our pain.

**"Memory safety on untrusted input."** A shell parses arbitrary command
lines — a huge adversarial surface. agentty funnels its untrusted input
(prompt, model output, tool results) through **one thin dynamism boundary**;
past it every value is typed (`std::expected<T,E>`, closed `variant` state
machines, tagged-type identities). The bugs that actually bit agentty in the
field were protocol and rendering-model logic bugs — the class Rust's borrow
checker does *not* catch — and agentty pins logic invariants at compile time.

**"Cargo dependency management."** A convenience, not a reason to rewrite a
working, disciplined codebase.

**"Rust's `#[test]` discipline."** This is the crux, and it's where agentty is
sharper: a Rust contributor proving a table correct writes `#[test]` and runs
them in CI. agentty writes `consteval` predicates and `static_assert`s them —
the proof runs **every build**, can't be skipped, and lives in the same header
as the table. Same discipline, smaller blast radius.

## The exhibit — proofs you can read in five minutes

- **`tool/policy.hpp`** — the permission matrix `(EffectSet × Profile) →
  Decision` is a pure `constexpr` function; every cell is `static_assert`ed.
- **`provider/error_class.hpp`** — `classify(HttpError) → ErrorClass` is
  exhaustive on the enum and every retry-relevant status is pinned
  (`{Status,429} == RateLimit`, `{Status,401} == Auth`).
- **`tool/spec.hpp`** — the capability catalog proves name↔Kind bijection,
  "only known tools carry Exec", truncation-strategy correlation, and the one
  sanctioned scheduling divergence for `task`, all `consteval`.
- **`domain/refined.hpp`** — refinement types: a `Bounded<uint16_t,1,65535>`
  port literally cannot hold 0 or 65536.
- **`util/env.hpp`** — the env-var enum and its string catalog are proven in
  bijection at build time.

## Beyond parity: two places agentty does *more* than stock Rust

- **Lock ordering is a type, not a comment.** The ACP server's mutexes are
  `RankedMutex<10>` (session) and `RankedMutex<20>` (thread); acquiring them
  out of order is a `static_assert` (compile error) in one scope and a
  debug-time `std::abort` tripwire across function boundaries. **Rust's type
  system does not check lock ordering at all** — deadlocks are memory-safe
  there.
- **Worker panics are structurally isolated.** Turn workers spawn through a
  primitive whose body makes `std::terminate` *unreachable* — one bad turn
  can never take down every session. The guarantee is in the spawn primitive,
  so it can't be forgotten per-site.

## When moving *would* be right

Honesty matters or the argument is worthless. Move to Rust if you want a large
external plugin ecosystem and C++ is scaring contributors away, if you start
hitting genuine data races the borrow checker would prevent, or if you're
doing a clean-slate rewrite anyway. None of those is true today.

:::tip
The full contract lives in the repo at `docs/DESIGN.md`; the honest
opposition research (every point a Rust advocate can raise, and how each was
closed) is in `docs/RUST-CRITIQUE.md`. The code is the argument — read the
five headers above.
:::
