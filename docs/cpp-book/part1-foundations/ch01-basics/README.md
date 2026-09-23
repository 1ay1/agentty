# Chapter 1: Types, Values, and References

This is the longest chapter in the book. Everything later stands on it.

**Time:** 6–10 hours if you run the code and do the exercises. Less if you
only read, but then you won't learn it.

**Prerequisite:** you can write a loop and call a function in some
language. That's it.

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

---

## how to read this

Every claim in these pages is backed by a program in `code/`. Build them
all first:

```sh
cd code
make          # all ten
make run      # all ten, in order, with headers
```

Then keep the matching program open beside the prose. Every output block
in these files is pasted from an actual run, not typed by hand. If your
machine prints something different, that difference is interesting and you
should chase it.

Blocks marked `// ERROR:` are supposed to fail. Compile them anyway.
Reading compiler errors is half the skill.

Everything builds with:

```
-std=c++23 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined
```

Sanitizers on, always. They turn "mysterious crash next Tuesday" into
"line 47, use-after-free, here's the stack".

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
