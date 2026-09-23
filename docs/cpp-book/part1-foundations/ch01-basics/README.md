# Chapter 1: Types, Values, and References

This is the longest chapter in the book. Everything later stands on it.

**Time:** 6–10 hours if you type the code and do the exercises. Less if
you only read, but then you won't learn it.

**Prerequisite:** you can write a loop and call a function in some
language. That's it.

**How it works:** every section builds a complete program in the prose,
piece by piece, with each line explained as it appears. You type it, run
it, then break it on purpose. The full source of every program is in the
section itself — you never need to open another file to follow along.

**What you'll be able to do at the end:** read
`include/agentty/domain/id.hpp`, `include/agentty/domain/lazy_bytes.hpp`,
and the `ImageContent` class in `include/agentty/domain/conversation.hpp`
line by line, and explain why every keyword in them is there.

---

## the sections

Read them in order. Each one has a program next to it in `code/`.

| # | section | what it fixes | time |
|---|---------|---------------|------|
| 0 | [The model you need first](00-the-model.md) | thinking in objects and storage instead of "variables" | 20m |
| 1 | [What a type actually is](01-what-is-a-type.md) | size, layout, operations, why `7/2` is `3` | 45m |
| 2 | [Integers lie to you](02-integers-lie.md) | wrap, UB, narrowing, the four traps | 45m |
| 3 | [Strong types: agentty's `Id<Tag>`](03-strong-types.md) | how a phantom tag stops swapped arguments | 75m |
| 4 | [Value categories](04-value-categories.md) | lvalue, prvalue, xvalue, what `std::move` really is | 60m |
| 5 | [Initialisation](05-initialisation.md) | five forms, narrowing, the `vector{3,7}` gotcha | 45m |
| 6 | [References and const](06-references-and-const.md) | passing, const views, `mutable` and why LazyBytes uses it | 75m |
| 7 | [Lifetime](07-lifetime.md) | when objects die, four ways to dangle | 75m |
| 8 | [Copy, move, elision](08-copy-move-elision.md) | who copies, who moves, why `noexcept` changes vector | 75m |
| 9 | [auto and decltype](09-auto-and-decltype.md) | what auto drops, the range-for copy | 45m |
| 10 | [Capstone: rebuild ImageContent](10-capstone-imagecontent.md) | all of it at once, on real agentty code | 90m |
| — | [Exercises](exercises.md) | eight, ordered by difficulty | 2h+ |
| — | [Quick reference](quick-reference.md) | one page, for after | — |

Plus two programs with no prose section, because they exist to be *run*:

- `code/11_zero_overhead.cpp` — proves the strong type is free, by diffing
  `-O2` assembly. Used by [section 3](03-strong-types.md).
- `code/12_selftest.cpp` — **every claim in this chapter as an
  assertion.** ~50 `static_assert`s and 15 runtime checks. If it builds
  and runs clean, the chapter is true on your machine.

---

## verify the chapter before you trust it

```sh
cd code && make verify
```

That does three things:

1. builds all twelve programs with `-Wall -Wextra -Wpedantic -Werror` and
   both sanitizers — **zero warnings allowed**
2. runs the self-test, which asserts every claim the prose makes
3. proves the zero-overhead claim by diffing optimised assembly

```
building all 12 programs, warnings are errors...
  ok: zero warnings

running the self-test...
  ...
15 runtime checks, 0 failures
everything the chapter claims is true on this machine.

proving zero overhead...
  len     identical     2 instructions, same order
  empty   identical     3 instructions, same order
  total   equivalent   12 instructions, scheduled differently
no pair costs an extra instruction. the strong type is free.

chapter 1 verified.
```

If something fails on your compiler or platform, that's genuinely
interesting — the failing assertion names the section it came from.

---

## how to read this

**Type the code.** Every section builds a program in front of you, line by
line, explaining each decision as it lands. Don't copy-paste and don't
just read — open an empty `.cpp` file and type it. Typing is slow enough
that you actually notice what you're writing.

Each section ends with a **now break it** list: deliberate changes that
make the program fail in instructive ways. Do them. Watching `-Wnarrowing`
fire on your own code teaches more than reading that it exists.

Compile everything with:

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined \
    yourfile.cpp -o yourfile && ./yourfile
```

Sanitizers on, always. They turn "mysterious crash next Tuesday" into
"line 47, use-after-free, here's the stack".

Every output block, every compiler error, and every warning quoted in
these pages is pasted from a real run on a real machine. If yours differs,
that's interesting and you should chase it.

### the reference copies in `code/`

`code/` holds a finished copy of each program, plus two extras that exist
only to be run. Use them to check your work **after** you've written your
own — not instead of writing it.

```sh
cd code
make run      # build and run all twelve
make verify   # the honest-chapter check, see below
```

---

## the three ideas

If you only remember three things from this chapter:

1. **An object is bytes with a type and a lifetime.** A name is a way to
   reach it. Those are different things, and most C++ confusion is the two
   getting mixed up.

2. **The type system is free.** `Id<ThreadIdTag>` is the same 32 bytes as
   `std::string` and compiles to the same machine code, but it makes
   `cancel(call_id, thread_id)` a compile error instead of a 3am page.

3. **Ownership and lifetime are design, not bookkeeping.** `const&` means
   "I'm borrowing", by-value means "I'm keeping", `string_view` means "I'm
   borrowing and I'll be gone before you are". Say which one you mean in
   the signature and the compiler enforces it.

---

## next

[Chapter 2: Memory Management — RAII and Ownership](../ch02-memory/) takes
section 7's lifetime rules and turns them into a design discipline.
