[← what is a type](01-what-is-a-type.md) · [chapter index](README.md) · [next: strong types →](03-strong-types.md)

# 2. Integers lie to you

**Time:** 45 minutes
**Code:** [`code/02_integers.cpp`](code/02_integers.cpp)

```sh
cd code && make 02_integers && ./02_integers
```

Four traps. All four have shipped in real software. Two of them have
caused CVEs.

---

## trap 1: unsigned subtraction wraps

```
-- trap 1: size() - 1 on an empty container --
empty.size()      = 0
empty.size() - 1  = 18446744073709551615   <- not -1. it wrapped.
```

`std::vector::size()` returns `std::size_t`, which is unsigned. `0 - 1` on
an unsigned type doesn't give you −1, because unsigned types have no
negatives. It gives you the largest representable value.

So this loop, which looks completely reasonable:

```cpp
for (std::size_t i = 0; i < v.size() - 1; ++i)
    use(v[i], v[i + 1]);
```

runs 18 quintillion times when `v` is empty, and reads memory it doesn't
own on iteration 0. Under ASan it's an immediate crash. Without ASan it's
a corrupted heap and a segfault somewhere unrelated, an hour later.

**The fix: never subtract from an unsigned size. Add to the other side.**

```cpp
for (std::size_t i = 0; i + 1 < v.size(); ++i)
    use(v[i], v[i + 1]);
```

```
write it with the subtraction moved to the other side:
  pairs: (10,20) (20,30)    <- i + 1 < size(), always safe
```

Empty vector: `0 + 1 < 0` is false, loop doesn't run. Correct.

This transformation works for every `size() - k` comparison. Learn it as a
reflex.

---

## trap 2: signed and unsigned don't compare

```
-- trap 2: -1 < 1u is false --
s < u            -> false   WRONG
std::cmp_less    -> true   right (C++20, compares values)
reason: s converts to unsigned first, becoming 4294967295.
```

When you compare a signed and an unsigned of the same rank, the *usual
arithmetic conversions* convert the signed one to unsigned. `-1` becomes
`4294967295`. Then `4294967295 < 1` is honestly false.

This is why `-Wsign-compare` exists, and why the program suppresses it
with a local pragma around exactly that line — the warning is the lesson.
Your compiler catches this one for free. Never turn that warning off.

Three ways out, best first:

```cpp
// 1. C++20: compares mathematical values, immune to conversions
if (std::cmp_less(s, u)) { }

// 2. don't mix. pick one signedness at the boundary and stick to it.
if (s < static_cast<int>(u)) { }      // only if u fits in int

// 3. use signed sizes internally
const auto n = static_cast<std::ptrdiff_t>(v.size());
for (std::ptrdiff_t i = 0; i < n; ++i) { }
```

`<utility>` also gives you `cmp_greater`, `cmp_less_equal`,
`cmp_not_equal`, and friends. And `std::in_range<int>(x)` tells you
whether a value fits in a target type before you convert.

### why is size() unsigned anyway

Historical. It predates the view that unsigned means "the value can't be
negative" versus "I want modular arithmetic". The standard committee has
publicly called it a mistake, but there's no fixing it without breaking
every program ever written. C++20 added `std::ssize(v)` which gives you a
signed size, and it's the nicer choice in new code:

```cpp
for (auto i = 0z; i < std::ssize(v) - 1; ++i)   // signed, subtraction safe
```

---

## trap 3: signed overflow is undefined, unsigned overflow is not

```
-- trap 3: signed overflow is UB, not wrap --
INT_MAX          = 2147483647
INT_MAX + 1 is UNDEFINED. the optimiser is allowed to assume
it never happens, so `if (x + 1 < x)` can be deleted entirely.

unsigned overflow IS defined (it wraps mod 2^N):
  UINT_MAX + 1 = 0
```

These are genuinely different rules and the difference matters.

**Unsigned overflow wraps.** `UINT_MAX + 1 == 0`, guaranteed, on every
conforming implementation. It's arithmetic mod 2^N. You can rely on it.

**Signed overflow is undefined behaviour.** Not "wraps to INT_MIN".
Undefined. The compiler is allowed to assume it never happens and
optimise on that assumption. Which means this check:

```cpp
if (x + 1 < x) { /* overflow! */ }        // gets DELETED
```

gets compiled away to nothing, because the optimiser reasons "`x + 1 < x`
implies overflow, overflow is UB, UB can't happen, therefore this branch
is dead". You get no check and no warning.

**Check before, not after:**

```cpp
if (x > std::numeric_limits<int>::max() - 1) {
    // adding 1 would overflow. handle it here.
}
```

Or use the builtins, which are exact and fast:

```cpp
int result;
if (__builtin_add_overflow(a, b, &result)) {
    // overflowed. result is unspecified.
}
```

C++26 adds `std::add_sat` / `std::mul_sat` for saturating arithmetic and
`std::ckd_add` for checked. Until then, the builtins are the portable-ish
answer on gcc and clang.

**Why is it UB at all?** So the compiler can assume `i + 1 > i` in a loop
and keep the induction variable in a register, vectorise, and strength-
reduce. Real performance, paid for with a real footgun.

---

## trap 4: narrowing is silent

```
-- trap 4: silent truncation --
uint8_t(300)     = 44   <- lost the top bits
int(3.99)        = 3   <- truncates toward zero
with braces, uint8_t b{300} and int i{3.99} are COMPILE ERRORS.
```

`300` is `0b100101100`. A `uint8_t` keeps the low 8 bits: `0b00101100` =
44. No warning with `=` assignment, no runtime error, just a wrong number.

`3.99` to `int` truncates toward zero. Not rounds. `int(-3.99)` is `-3`,
not `-4`.

**Braces make it a compile error:**

```cpp
std::uint8_t a = 300;    // compiles. gives you 44.
std::uint8_t b{300};     // ERROR: narrowing conversion

int c = 3.99;            // compiles. gives you 3.
int d{3.99};             // ERROR: narrowing conversion
```

That's the single strongest argument for brace initialisation, and it's
what section 5 is about.

When you genuinely want truncation, say so:

```cpp
auto byte = static_cast<std::uint8_t>(value & 0xFF);   // explicit
auto n    = static_cast<int>(std::lround(d));          // explicit rounding
```

A `static_cast` there is documentation. It tells the next reader "yes, I
know, I meant it."

---

## the rules, compressed

- Never subtract from an unsigned size. Rewrite `i < size() - 1` as
  `i + 1 < size()`.
- Never compare signed to unsigned. Use `std::cmp_less` or fix the types.
- Never let signed arithmetic overflow. Check before the operation.
- Use `{}` so narrowing is a compile error.
- Use `<cstdint>` fixed-width types at every boundary.
- Keep `-Wall -Wextra` on. Two of these four are caught for free.

---

## where this bites in agentty

Anywhere an index walks a `std::vector<Message>`. The rendering path
computes windows into the message list, and every one of those is written
as `i + n < msgs.size()` rather than `i < msgs.size() - n` for exactly
this reason. A thread with zero messages is a completely normal state (a
fresh thread), so the empty case isn't hypothetical, it's the first thing
that happens on startup.

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
   that's UB.

</details>

---

[next: strong types, agentty's Id<Tag> →](03-strong-types.md)
