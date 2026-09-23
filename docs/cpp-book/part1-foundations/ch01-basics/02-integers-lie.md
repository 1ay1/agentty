[← what is a type](01-what-is-a-type.md) · [chapter index](README.md) · [next: strong types →](03-strong-types.md)

# 2. Integers lie to you

**Time:** 45 minutes. Type everything.

Four traps. All four have shipped in real software. Two of them have
caused CVEs. You're going to write each one, watch it misbehave, then fix
it.

Open `02_integers.cpp`.

---

## trap 1: unsigned subtraction wraps

Start here:

```cpp
#include <cstdio>
#include <vector>

static void unsigned_wrap() {
    std::puts("-- trap 1: size() - 1 on an empty container --");
    std::vector<int> empty;
    std::printf("empty.size()      = %zu\n", empty.size());
    std::printf("empty.size() - 1  = %zu   <- not -1. it wrapped.\n",
                empty.size() - 1);
}

int main() {
    unsigned_wrap();
}
```

```
-- trap 1: size() - 1 on an empty container --
empty.size()      = 0
empty.size() - 1  = 18446744073709551615   <- not -1. it wrapped.
```

`std::vector::size()` returns `std::size_t`, which is **unsigned**. `0 - 1`
on an unsigned type doesn't give you −1, because unsigned types have no
negatives. It gives you the largest representable value: 2⁶⁴−1.

### why this is dangerous

The bug doesn't look like the above. It looks like this, which is code you
would write without thinking:

```cpp
for (std::size_t i = 0; i < v.size() - 1; ++i)
    use(v[i], v[i + 1]);          // walk adjacent pairs
```

Perfectly reasonable. Walk every adjacent pair. Except when `v` is empty:
`v.size() - 1` is 18 quintillion, so the loop runs essentially forever,
and on iteration 0 it already reads `v[0]` and `v[1]` on an empty vector.

Under ASan that's an immediate crash with a clean report. Without ASan
it's a corrupted heap and a segfault somewhere unrelated, an hour later.

### the fix

Add this to the function:

```cpp
    std::puts("\nso this loop runs 18 quintillion times:");
    std::puts("  for (size_t i = 0; i < v.size() - 1; ++i)   // BAD");
    std::puts("write it with the subtraction moved to the other side:");
    std::vector<int> v{10, 20, 30};
    std::printf("  pairs: ");
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        std::printf("(%d,%d) ", v[i], v[i + 1]);
    std::puts("   <- i + 1 < size(), always safe");
```

```
write it with the subtraction moved to the other side:
  pairs: (10,20) (20,30)    <- i + 1 < size(), always safe
```

**Never subtract from an unsigned size. Add to the other side instead.**

`i + 1 < v.size()`. On an empty vector that's `0 + 1 < 0`, which is false,
so the loop doesn't run. Correct, and there's no special case to remember.

This transformation works for any `size() - k` comparison:

```cpp
i <  size() - 1      →   i + 1 <  size()
i <= size() - 1      →   i + 1 <= size()
i <  size() - k      →   i + k <  size()
```

Learn it as a reflex. You'll use it constantly.

---

## trap 2: signed and unsigned don't compare

Add a new function:

```cpp
#include <utility>     // for std::cmp_less. add this up top.

static void mixed_compare() {
    std::puts("\n-- trap 2: -1 < 1u is false --");
    int      s = -1;
    unsigned u = 1;

    std::printf("s < u            -> %s   WRONG\n", (s < u) ? "true" : "false");
    std::printf("std::cmp_less    -> %s   right (C++20, compares values)\n",
                std::cmp_less(s, u) ? "true" : "false");
    std::puts("reason: s converts to unsigned first, becoming 4294967295.");
}
```

Compile it. **Look at the warning:**

```
warning: comparison of integer expressions of different signedness:
'int' and 'unsigned int' [-Wsign-compare]
```

That warning is the entire lesson. Your compiler already knows this line
is wrong.

```
-- trap 2: -1 < 1u is false --
s < u            -> false   WRONG
std::cmp_less    -> true   right (C++20, compares values)
reason: s converts to unsigned first, becoming 4294967295.
```

### what actually happened

When you compare a signed and an unsigned of the same rank, the **usual
arithmetic conversions** kick in and convert the signed operand to
unsigned. So `-1` becomes `4294967295`, and `4294967295 < 1` is honestly
false.

The compiler isn't being perverse. It's following a rule from C that made
sense on hardware from 1972. But the result is a comparison that silently
gives the wrong answer.

### keeping the warning visible

If you want to keep this demo in a build that treats warnings as errors,
suppress it *locally* and say why:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    std::printf("s < u            -> %s   WRONG\n", (s < u) ? "true" : "false");
#pragma GCC diagnostic pop
```

The `push`/`pop` pair means you've turned the warning off for exactly one
line and turned it straight back on. **Never disable a warning
file-wide.** If you find yourself doing that, the warning is telling you
something and you're gagging it.

### three ways out, best first

```cpp
// 1. C++20: compares mathematical values, immune to conversions
if (std::cmp_less(s, u)) { }

// 2. don't mix. pick a signedness at the boundary and stick to it.
if (s < static_cast<int>(u)) { }      // only valid if u fits in int

// 3. use signed sizes internally
const auto n = static_cast<std::ptrdiff_t>(v.size());
for (std::ptrdiff_t i = 0; i < n; ++i) { }
```

`<utility>` also gives you `cmp_greater`, `cmp_less_equal`,
`cmp_not_equal` and friends. And `std::in_range<int>(x)` tells you whether
a value fits in a target type *before* you convert it.

### why is `size()` unsigned anyway

Historical accident. It predates the modern view that unsigned should mean
"I want modular arithmetic", not "this value can't be negative". Members
of the standards committee have publicly called it a mistake, but it can't
be fixed without breaking every program ever written.

C++20 added `std::ssize(v)`, which gives you a **signed** size, and it's
the nicer choice in new code:

```cpp
#include <iterator>
for (auto i = std::ssize(v) - 1; i >= 0; --i)   // counting down works now
```

That countdown loop is impossible to write correctly with an unsigned
index, which tells you something.

---

## trap 3: signed overflow is undefined, unsigned overflow is not

```cpp
#include <limits>     // add up top

static void signed_overflow() {
    std::puts("\n-- trap 3: signed overflow is UB, not wrap --");
    std::printf("INT_MAX          = %d\n", std::numeric_limits<int>::max());
    std::puts("INT_MAX + 1 is UNDEFINED. the optimiser is allowed to assume");
    std::puts("it never happens, so `if (x + 1 < x)` can be deleted entirely.");
    std::puts("check before you add:");
    int x = std::numeric_limits<int>::max();
    bool safe = x <= std::numeric_limits<int>::max() - 1;
    std::printf("  can add 1 to INT_MAX? %s\n", safe ? "yes" : "no");

    std::puts("\nunsigned overflow IS defined (it wraps mod 2^N):");
    unsigned umax = std::numeric_limits<unsigned>::max();
    std::printf("  UINT_MAX + 1 = %u\n", umax + 1u);
}
```

```
-- trap 3: signed overflow is UB, not wrap --
INT_MAX          = 2147483647
...
  can add 1 to INT_MAX? no

unsigned overflow IS defined (it wraps mod 2^N):
  UINT_MAX + 1 = 0
```

These are genuinely different rules, and the difference matters.

**Unsigned overflow wraps.** `UINT_MAX + 1 == 0`, guaranteed, on every
conforming implementation. It's arithmetic mod 2ᴺ. You can rely on it —
hash functions and checksums do exactly that.

**Signed overflow is undefined behaviour.** Not "wraps to INT_MIN".
*Undefined.* The compiler may assume it never happens.

### the check that gets deleted

This is the part that catches people. You write an overflow check:

```cpp
if (x + 1 < x) {
    // overflow! handle it
}
```

and the optimiser reasons: "`x + 1 < x` can only be true if signed
overflow occurred. Signed overflow is UB. UB never happens. Therefore this
condition is always false. Delete the branch."

You get no check and no warning. Try it yourself — compile that at `-O2`
and look at the assembly.

**Check before the operation, not after:**

```cpp
if (x > std::numeric_limits<int>::max() - 1) {
    // adding 1 would overflow. handle it here.
}
```

Or use the builtins, which are exact and compile to a single instruction
plus a branch on the overflow flag:

```cpp
int result;
if (__builtin_add_overflow(a, b, &result)) {
    // overflowed. result is unspecified.
}
```

C++26 adds `std::add_sat` / `std::mul_sat` for saturating arithmetic and
`std::ckd_add` for checked. Until then, the builtins are the practical
answer on gcc and clang.

**Why is it UB at all?** So the compiler can assume `i + 1 > i` inside a
loop, which lets it keep the induction variable in a register, vectorise,
and strength-reduce. Real performance, paid for with a real footgun.

UBSan catches it at runtime, which is one more reason to keep
`-fsanitize=undefined` on in debug builds.

---

## trap 4: narrowing is silent

```cpp
#include <cstdint>     // add up top

static void narrowing() {
    std::puts("\n-- trap 4: silent truncation --");
    int big = 300;
    std::uint8_t byte = static_cast<std::uint8_t>(big);
    std::printf("uint8_t(300)     = %u   <- lost the top bits\n",
                static_cast<unsigned>(byte));
    double d = 3.99;
    std::printf("int(3.99)        = %d   <- truncates toward zero\n",
                static_cast<int>(d));
    std::puts("with braces, uint8_t b{300} and int i{3.99} are COMPILE ERRORS.");
    std::puts("that is the main reason to prefer {} init. see 05_init.");
}
```

```
-- trap 4: silent truncation --
uint8_t(300)     = 44   <- lost the top bits
int(3.99)        = 3   <- truncates toward zero
```

`300` in binary is `100101100`. A `uint8_t` keeps the low 8 bits,
`00101100`, which is 44. No warning with `=` assignment, no runtime error,
just a wrong number.

`3.99` to `int` **truncates toward zero**, it doesn't round. And note
`int(-3.99)` is `-3`, not `-4` — toward zero, not toward negative
infinity.

### braces turn it into a compile error

Try adding these to your file, one at a time:

```cpp
std::uint8_t a = 300;    // compiles. gives you 44.
std::uint8_t b{300};     // ERROR: narrowing conversion

int c = 3.99;            // compiles. gives you 3.
int d{3.99};             // ERROR: narrowing conversion
```

```
error: narrowing conversion of '300' from 'int' to 'std::uint8_t'
{aka 'unsigned char'} [-Wnarrowing]
```

That difference is the single strongest argument for brace initialisation,
and it's what section 5 is about.

When you genuinely want truncation, say so:

```cpp
auto byte = static_cast<std::uint8_t>(value & 0xFF);   // explicit mask
auto n    = static_cast<int>(std::lround(d));          // explicit rounding
```

A `static_cast` there is documentation. It tells the next reader "yes, I
know this loses information, I meant it."

---

## the whole file

```cpp
// 02_integers.cpp — the four integer traps that bite real code.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>   // std::cmp_less
#include <vector>

// ── trap 1: unsigned subtraction wraps ─────────────────────────────────
static void unsigned_wrap() {
    std::puts("-- trap 1: size() - 1 on an empty container --");
    std::vector<int> empty;
    std::printf("empty.size()      = %zu\n", empty.size());
    std::printf("empty.size() - 1  = %zu   <- not -1. it wrapped.\n",
                empty.size() - 1);

    std::puts("\nso this loop runs 18 quintillion times:");
    std::puts("  for (size_t i = 0; i < v.size() - 1; ++i)   // BAD");
    std::puts("write it with the subtraction moved to the other side:");
    std::vector<int> v{10, 20, 30};
    std::printf("  pairs: ");
    for (std::size_t i = 0; i + 1 < v.size(); ++i)
        std::printf("(%d,%d) ", v[i], v[i + 1]);
    std::puts("   <- i + 1 < size(), always safe");
}

// ── trap 2: signed/unsigned comparison converts the signed one ─────────
static void mixed_compare() {
    std::puts("\n-- trap 2: -1 < 1u is false --");
    int      s = -1;
    unsigned u = 1;

    // INTENTIONAL WARNING. -Wsign-compare fires on the next line and that
    // warning is the entire lesson: the compiler knows this is wrong.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
    std::printf("s < u            -> %s   WRONG\n", (s < u) ? "true" : "false");
#pragma GCC diagnostic pop
    std::printf("std::cmp_less    -> %s   right (C++20, compares values)\n",
                std::cmp_less(s, u) ? "true" : "false");
    std::puts("reason: s converts to unsigned first, becoming 4294967295.");
}

// ── trap 3: signed overflow is undefined behaviour ─────────────────────
static void signed_overflow() {
    std::puts("\n-- trap 3: signed overflow is UB, not wrap --");
    std::printf("INT_MAX          = %d\n", std::numeric_limits<int>::max());
    std::puts("INT_MAX + 1 is UNDEFINED. the optimiser is allowed to assume");
    std::puts("it never happens, so `if (x + 1 < x)` can be deleted entirely.");
    std::puts("check before you add:");
    int x = std::numeric_limits<int>::max();
    bool safe = x <= std::numeric_limits<int>::max() - 1;
    std::printf("  can add 1 to INT_MAX? %s\n", safe ? "yes" : "no");

    std::puts("\nunsigned overflow IS defined (it wraps mod 2^N):");
    unsigned umax = std::numeric_limits<unsigned>::max();
    std::printf("  UINT_MAX + 1 = %u\n", umax + 1u);
}

// ── trap 4: narrowing on assignment is silent ──────────────────────────
static void narrowing() {
    std::puts("\n-- trap 4: silent truncation --");
    int big = 300;
    std::uint8_t byte = static_cast<std::uint8_t>(big);
    std::printf("uint8_t(300)     = %u   <- lost the top bits\n",
                static_cast<unsigned>(byte));
    double d = 3.99;
    std::printf("int(3.99)        = %d   <- truncates toward zero\n",
                static_cast<int>(d));
    std::puts("with braces, uint8_t b{300} and int i{3.99} are COMPILE ERRORS.");
    std::puts("that is the main reason to prefer {} init. see 05_init.");
}

int main() {
    unsigned_wrap();
    mixed_compare();
    signed_overflow();
    narrowing();
}
```

---

## the rules, compressed

- Never subtract from an unsigned size. Rewrite `i < size() - 1` as
  `i + 1 < size()`.
- Never compare signed to unsigned. Use `std::cmp_less` or fix the types.
- Never let signed arithmetic overflow. Check *before* the operation.
- Use `{}` so narrowing is a compile error.
- Use `<cstdint>` fixed-width types at every boundary.
- Keep `-Wall -Wextra` on. Two of these four are caught for free.

---

## where this bites in agentty

Anywhere an index walks a `std::vector<Message>`. The rendering path
computes windows into the message list, and every one of those is written
as `i + n < msgs.size()` rather than `i < msgs.size() - n` for exactly
this reason.

A thread with zero messages is a completely normal state — it's a fresh
thread, the first thing that exists at startup. So the empty case isn't
hypothetical, it's the default.

---

## now break it

1. Write the bad loop (`i < v.size() - 1`) over an empty vector. Build
   with `-fsanitize=address` and read the report. That report is what a
   real crash looks like.
2. Change `unsigned u = 1` to `unsigned long u = 1`. Does the warning
   still fire? Does the answer change?
3. Compile `if (x + 1 < x) puts("overflow");` at `-O0` and `-O2`, and
   diff the assembly. Find the branch disappearing.
4. Try `std::uint8_t b{255}` and `std::uint8_t b{256}`. Read the second
   error.
5. Predict what `static_cast<int>(-3.99)` gives before running it.

---

## check yourself

1. `std::vector<int> v;` — what does `v.size() - 1` evaluate to, and what
   type is it?
2. Is `-1 < 1u` true or false? Why?
3. Which is UB: `INT_MAX + 1` or `UINT_MAX + 1`?
4. Why can the compiler delete `if (x + 1 < x) overflow();`?
5. `std::uint8_t x{200}; x += 100;` — what is `x`, and is it UB?

<details>
<summary>answers</summary>

1. `18446744073709551615` on a 64-bit machine, of type `std::size_t`.
2. False. `-1` converts to unsigned and becomes 4294967295.
3. `INT_MAX + 1` is UB. `UINT_MAX + 1` is defined and wraps to 0.
4. Because `x + 1 < x` can only be true on signed overflow, signed
   overflow is UB, and the optimiser assumes UB never happens.
5. 44, and it's **not** UB. `x` promotes to `int` for the addition (300
   fits fine), then the conversion back to `uint8_t` is a defined
   truncation. Unsigned narrowing is well-defined; it's *signed overflow*
   that's UB. This one catches almost everybody.

</details>

---

[next: strong types, agentty's Id<Tag> →](03-strong-types.md)
