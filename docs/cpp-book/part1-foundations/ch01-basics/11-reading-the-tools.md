[← capstone](10-capstone-imagecontent.md) · [chapter index](README.md) · [next: exercises →](exercises.md)

# 11. Reading what the tools tell you

**Time:** 60 minutes. Type everything.

Every section so far ended with "now break it". This one is about the
other half of that: **what the machine says back**, and how to read it
fast.

This is the section that separates someone who knows C++ from someone who
can *work* in C++. The rules you've learned tell you what's wrong. The
tools tell you *where*, and learning to read them is a skill you practise,
not a fact you memorise.

---

## the ladder

Four tools, cheapest first. Always go in this order.

| tool | catches | cost | when |
|---|---|---|---|
| compiler warnings | type confusion, obvious lifetime bugs | free | every build |
| `static_assert` | wrong assumptions about types | free | when you think you know |
| sanitizers | memory and UB, on paths you ran | ~2x runtime | every debug build |
| debugger | everything else | your time | when the above go quiet |

The mistake beginners make is reaching for the debugger first. The mistake
intermediates make is not turning the first three on at all.

---

## rung 1: warnings are not style

Most C++ warnings are bugs wearing a polite hat. Here's the set that
matters, and what each one actually means:

| warning | it means |
|---|---|
| `-Wsign-compare` | your comparison gives the wrong answer (§2) |
| `-Wnarrowing` | you're silently losing data (§2) |
| `-Wreturn-local-addr` | you returned a reference to a dead object (§7) |
| `-Wdangling-pointer` | you saved a pointer to something that died (§7) |
| `-Wpessimizing-move` | your `std::move` made it slower (§8) |
| `-Wreorder` | your init list runs in a different order than it reads (§5) |
| `-Wunused-result` | you discarded a `[[nodiscard]]` answer (§3) |
| `-Wtype-limits` | a comparison that can never be true |

**None of those are opinions.** Each is the compiler saying "the code you
wrote does not do what it looks like it does".

Turn them on and keep them on:

```sh
-Wall -Wextra -Wpedantic
```

And for code you control, consider going further:

```sh
-Werror              # warnings become errors. no "I'll fix it later".
-Wshadow             # a local hiding an outer name
-Wconversion         # every implicit narrowing, not just in braces
```

`-Wconversion` is noisy on existing code but excellent on new code — it
catches the §2 truncations that `{}` would have caught, everywhere, not
just at initialisation.

### the one thing to never do

```cpp
#pragma GCC diagnostic ignored "-Wsign-compare"   // at file scope. NO.
```

If you must silence a warning, silence it for **one line**, with a comment
saying why:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    // deliberate: this demo exists to SHOW the broken comparison
    bad = (s < u);
#pragma GCC diagnostic pop
```

That's what sections 2, 4 and 8 do. `push`/`pop` means the warning is off
for exactly as long as you meant and no longer.

---

## rung 2: `static_assert` your assumptions

You've been using this. The habit worth forming is: **when you find
yourself thinking "I'm pretty sure this type is X", stop guessing and ask
the compiler.**

```cpp
#include <type_traits>

static_assert(sizeof(ThreadId) == sizeof(std::string));
static_assert(std::is_nothrow_move_constructible_v<Message>);
static_assert(std::is_same_v<decltype(v.size()), std::size_t>);
static_assert(std::is_aggregate_v<Point>);
```

Three reasons this is better than checking in a debugger:

1. It runs at compile time, so it costs nothing.
2. It's **permanent**. When someone adds a destructor six months from now
   and kills your move (§8), the build breaks that day.
3. The failure message is a sentence you wrote, so future-you knows what
   the assumption was for.

Write the message as an explanation, not a restatement:

```cpp
// bad
static_assert(std::is_nothrow_move_constructible_v<Message>, "not nothrow");

// good
static_assert(std::is_nothrow_move_constructible_v<Message>,
              "vector<Message> must MOVE on reallocation, not copy. "
              "if this fires, someone declared a destructor.");
```

### make the compiler print a type

There's no `typeof` you can print. But you can force the compiler to tell
you, by declaring a template you never define:

```cpp
template <typename T> struct TypeIs;      // declared, never defined

auto x = some_complicated_expression();
TypeIs<decltype(x)> probe;                // deliberate error
```

```
// gcc
error: aggregate 'TypeIs<long unsigned int> probe' has incomplete type
and cannot be defined

// clang
error: implicit instantiation of undefined template 'TypeIs<unsigned long>'
```

There it is: `long unsigned int`. The error message *is* the output. It's
the fastest way to settle "what type is this actually", and it works for
any expression — a lambda, a deduced return, a nested template
instantiation you'd never be able to write out by hand.

Section 2 used the tidier version — a `SHOW` macro with explicit
specialisations — when the answer needed to be printed at runtime. Use
whichever fits.

---

## rung 3: reading a sanitizer report

This is the skill. Most people see an ASan wall of text and scroll past
it. It's actually a well-structured document and it tells you everything.

Write this bug — it's trap 1 from §2, in a form ASan can catch cleanly:

```cpp
#include <cstdio>
#include <cstdlib>

static int sum_pairs(const int* v, std::size_t n) {
    int total = 0;
    for (std::size_t i = 0; i < n - 1; ++i)   // n == 0 -> wraps
        total += v[i] + v[i + 1];
    return total;
}

int main() {
    int* a = (int*)std::malloc(0);
    std::printf("%d\n", sum_pairs(a, 0));
    std::free(a);
}
```

```sh
g++ -std=c++23 -O0 -g -fsanitize=address dbg.cpp -o dbg && ./dbg
```

```
=================================================================
==232296==ERROR: AddressSanitizer: heap-buffer-overflow on address
0x7b4a115e0010 at pc 0x557be2cd2218 bp 0x7fffba432220 sp 0x7fffba432210
READ of size 4 at 0x7b4a115e0010 thread T0
    #0 0x557be2cd2217 in sum_pairs /tmp/dbg3.cpp:6
    #1 0x557be2cd22b3 in main /tmp/dbg3.cpp:11
    #2 0x7f2a12427780  (/usr/lib/libc.so.6+0x27780)
    #3 0x7f2a124278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8)
    #4 0x557be2cd20d4 in _start (/tmp/dbg3+0x10d4)

0x7b4a115e0011 is located 0 bytes after 1-byte region
[0x7b4a115e0010,0x7b4a115e0011)
allocated by thread T0 here:
    #0 0x7f2a12d2c0c1 in malloc (/usr/lib/libasan.so.8+0x12c0c1)
    #1 0x557be2cd229e in main /tmp/dbg3.cpp:10
    #2 0x7f2a12427780  (/usr/lib/libc.so.6+0x27780)

SUMMARY: AddressSanitizer: heap-buffer-overflow /tmp/dbg3.cpp:6 in sum_pairs
```

### read it in this order

**1. The last line first.** `SUMMARY` gives you the bug class and the
exact file:line. Start here, always. `dbg3.cpp:6 in sum_pairs`.

**2. The bug class.** `heap-buffer-overflow` — you read past the end of a
heap allocation. The vocabulary is small and worth knowing:

| class | means |
|---|---|
| `heap-buffer-overflow` | read/wrote past the end of a `new`/`malloc` block |
| `stack-buffer-overflow` | same, on a local array |
| `heap-use-after-free` | touched memory that was freed (§7 shapes 2, 3, 4) |
| `stack-use-after-scope` | touched a local after its scope ended (§7 shape 1) |
| `stack-use-after-return` | returned a pointer/reference to a local (§7 shape 1) |
| `double-free` | freed twice \u2014 usually a missing move ctor (§8) |
| `memory leak` | never freed |

**3. What you did, and where.** `READ of size 4` — a 4-byte read, which is
an `int`. Frame `#0` is the exact line. Ignore frames pointing into
`libc.so` or `libasan.so`; your code is the first frame with a filename
you recognise.

**4. The geometry.** This line is the one people skip and it's the most
useful:

```
0x7b4a115e0011 is located 0 bytes after 1-byte region [0x...10, 0x...11)
```

"0 bytes **after**" means you ran off the end by exactly one element. If
it said "4 bytes after", you'd be two `int`s past. If it said "8 bytes
**before**", you'd have a negative index. That distance tells you *how
wrong* your index was, which usually points straight at the arithmetic.

**5. The allocation stack.** Where the memory came from — `main`, line 10.
For a use-after-free there's a third stack, "freed by thread T0 here",
telling you where it died. Those three stacks together — allocated, freed,
used — are the whole story of the bug.

### the other sanitizers

```sh
-fsanitize=undefined     # signed overflow, bad shifts, misaligned access,
                         # invalid enum values, null deref
-fsanitize=thread        # data races. cannot combine with address.
-fsanitize=leak          # included in address by default on Linux
```

UBSan's output is terser and points at the exact operation:

```
runtime error: signed integer overflow: 2147483647 + 1 cannot be
represented in type 'int'
```

Run both address and undefined together. They compose.

```sh
-fsanitize=address,undefined -fno-omit-frame-pointer -g
```

`-g` gives you line numbers. `-fno-omit-frame-pointer` gives you readable
stacks. Without them you get hex addresses and a bad afternoon.

### the standard library has its own checks

You may find a bug gets caught *before* ASan sees it:

```
/usr/include/c++/16/bits/stl_vector.h:1272: ... Assertion '__n < this->size()' failed.
```

That's `_GLIBCXX_ASSERTIONS`, libstdc++'s own bounds checking on
`operator[]`, `front()`, `back()` and friends. Many distros enable it by
default in debug builds. It's cheaper than ASan and often more precise —
it names the *precondition* you violated rather than the memory you
touched.

Turn it on explicitly if your setup doesn't:

```sh
-D_GLIBCXX_ASSERTIONS        # libstdc++
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE   # libc++
```

---

## rung 4: the debugger, used well

By the time you get here the first three have gone quiet, which means the
bug is logic, not memory. Three things worth knowing beyond `break` and
`print`.

**Conditional breakpoints**, so you don't hit `continue` four hundred
times:

```
(gdb) break message.cpp:112 if id.value == "msg-77"
```

**Watchpoints**, for "who is changing this":

```
(gdb) watch obj.field_
Hardware watchpoint 2: obj.field_
(gdb) continue
```

The debugger stops at the instruction that wrote it, with a stack. This is
the single fastest way to find an unexpected mutation, and most people
never learn it.

**Reverse execution**, when you've gone one step too far:

```
(gdb) record
(gdb) reverse-step
```

Slow, but it beats restarting and re-triggering a rare path.

---

## a worked example

Here's the loop you'd actually run. Someone reports agentty crashes when
opening one particular thread.

**1. Reproduce under sanitizers.** Not under the normal build.

```sh
cmake --preset debug && ./build/agentty --thread=the-bad-one
```

**2. Read the SUMMARY line.** Say it's
`heap-use-after-free ... in render_message`.

**3. Read the three stacks.** Allocated in `load_thread`, freed in
`compact_thread`, used in `render_message`. **That's the bug, stated.**
Something kept a pointer across a compaction.

**4. Match it to a shape.** §7 shape 3: a reference into a container that
was modified. Compaction removes messages, which invalidates everything
after the removal point.

**5. Fix it at the type level if you can.** The fix isn't "re-fetch the
pointer". It's "don't key by position" — which is exactly the reasoning
behind `MessageId` in §3. A bug that keeps coming back is a design
problem wearing a bug costume.

Notice step 5. Rungs 1–4 find *this* bug. Asking "what type change makes
this class of bug unrepresentable" is what stops the next one.

---

## now break it

1. Write the `sum_pairs` bug above and read the full ASan report. Identify
   all five parts.
2. Change `malloc(0)` to `malloc(4)` and pass `n = 1`. Does it still fire?
   What does "0 bytes after" become?
3. Trigger each of the four §7 dangling shapes and note which sanitizer
   class each reports.
4. Write `TypeIs<decltype(v.size())> probe;` and read the type out of the
   error message.
5. Cause a `signed integer overflow` and read the UBSan message. Then
   compile the same code *without* `-fsanitize=undefined` at `-O2` and
   confirm it silently does something else.
6. Set a watchpoint on a member in gdb and catch a write.

---

## check yourself

1. In an ASan report, which line do you read first?
2. What does "0 bytes after a 1-byte region" tell you that the line
   number doesn't?
3. Why run `-fsanitize=address` and `-fsanitize=undefined` together but
   never `address` and `thread`?
4. What does `-Wpessimizing-move` mean, in terms of what your code does?
5. How do you make the compiler tell you a type you can't name?
6. You fixed a use-after-free by re-fetching a pointer. What question
   should you ask next?

<details>
<summary>answers</summary>

1. The `SUMMARY` line at the bottom — it has the bug class and the exact
   file:line.
2. How far off your index was. "0 bytes after" is off-by-one; "8 bytes
   after" is off-by-two `int`s; "before" means a negative index.
3. Address and undefined instrument different things and compose fine.
   ThreadSanitizer uses an incompatible shadow-memory layout, so it can't
   be combined with ASan.
4. That a `std::move` blocked copy elision, so your code performs a real
   move where it would otherwise have performed nothing at all.
5. Declare `template <typename T> struct TypeIs;` without defining it and
   instantiate it with the type. The compile error names it.
6. "What type change would make this class of bug unrepresentable?" A
   recurring bug is usually a design problem, not a coding mistake.

</details>

---

[next: exercises →](exercises.md)
