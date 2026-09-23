[← the model](00-the-model.md) · [chapter index](README.md) · [next: integers lie →](02-integers-lie.md)

# 1. What a type actually is

**Time:** 45 minutes
**Code:** [`code/01_types.cpp`](code/01_types.cpp)

```sh
cd code && make 01_types && ./01_types
```

---

## three things, not one

A type answers three questions at once:

1. **How many bytes?** and how must they be aligned
2. **How do I read those bytes?** the same 32 bits are a float or an int
   depending only on the type
3. **What can I do with it?** `+` on two ints is one instruction; `+` on
   two strings allocates

That's it. A type is not a class, not an object, not a box. It's a
promise about bytes plus a set of operations.

---

## question 1: how many bytes

```
-- sizes on this machine --
char         1 byte   align 1
short        2 bytes  align 2
int          4 bytes  align 4
long         8 bytes  align 8
float        4 bytes  align 4
double       8 bytes  align 8
void*        8 bytes  align 8
std::string 32 bytes  align 8
```

Only two of those are guaranteed by the standard. `sizeof(char)` is
**always** 1, and the ordering
`char ≤ short ≤ int ≤ long ≤ long long` always holds. Everything else is
your compiler and platform.

So when the size matters — a wire format, a file header, a protocol — use
the fixed-width types from `<cstdint>`:

```cpp
std::uint8_t  one_byte;
std::uint32_t four_bytes_unsigned;
std::int64_t  eight_bytes_signed;
```

agentty does this everywhere a number crosses a boundary:

```cpp
// include/agentty/domain/conversation.hpp
enum class Role : std::uint8_t { User, Assistant, System };

class ImageContent {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};
```

`Role` is pinned to one byte because it gets serialised. `width` is
pinned to 32 bits unsigned because an image dimension is never negative
and never needs 64 bits.

**Why `std::string` is 32 bytes** — it's a pointer, a size, a capacity,
and a small-string buffer, packed together. libstdc++ lays it out as
`{char* ptr; size_t size; union { char buf[16]; size_t cap; }}`. Short
strings live entirely inside the object with no heap allocation at all.
That's the *small string optimisation*, and it's why passing strings
around in C++ is cheaper than it looks.

---

## question 2: how to read the bytes

```
-- same 4 bytes, two types --
as uint32_t: 1092616192
as float   : 10.0
the bytes never changed. only the type we read them with did.
```

`0x41200000` is the number 1092616192 and also the float 10.0. Same 32
bits. The type is the only thing that decides which.

The code does this with `std::memcpy`:

```cpp
std::uint32_t bits = 0x41200000u;
float f;
std::memcpy(&f, &bits, sizeof f);
```

Not with a cast. `*reinterpret_cast<float*>(&bits)` is a *strict aliasing*
violation — undefined behaviour, and modern optimisers will genuinely
break your code over it. `memcpy` is the legal spelling, and every
compiler turns it into a single move instruction at `-O1` and above. In
C++20 and later you can also write `std::bit_cast<float>(bits)`, which is
the same thing with a nicer face and a `constexpr` guarantee.

---

## question 3: what you can do with it

```
-- / means different things --
7 / 2       = 3      (int division truncates)
7.0 / 2.0   = 3.5
(double)7/2 = 3.5    (one cast fixes the whole expression)
```

`/` is not one operation. On two ints it's integer division, which
truncates toward zero. On two doubles it's floating-point division. The
compiler picks based on the operand types, and the *result* type follows
from the operand types too — which is where the bug comes from.

### the percentage bug

```
-- a real percentage bug --
(done / total) * 100            = 0%   WRONG
(done * 100) / total            = 75%   ok for ints
(double(done) / total) * 100.0  = 75%   ok, and reads right
```

`3 / 4` is `0`. Then `0 * 100` is `0`. Every progress bar written by
someone new to C++ has this bug once.

Two fixes, and they're not equivalent:

- `(done * 100) / total` stays in integers. Correct, fast, but overflows
  if `done` is large. Fine for a progress bar, wrong for byte counts.
- `(double(done) / total) * 100.0` converts first. Correct for any
  magnitude, and it *reads* like what you meant.

Note where the cast goes. Casting one operand is enough: C++ promotes the
other to match. `static_cast<double>(a) / b` works; you don't need to cast
both.

---

## layout: order changes size

```
-- member order changes the size --
struct { char; int; char; } = 12 bytes
struct { int; char; char; } = 8 bytes
same three members. the compiler pads to keep int aligned.
```

The members are identical. Only the order changed, and the struct got 50%
smaller.

Here's why. An `int` must sit at an address divisible by 4.

```
struct Padded { char a; int b; char c; };

byte:  0    1    2    3    4    5    6    7    8    9   10   11
      [a] [pad][pad][pad][   b (4 bytes)   ] [c] [pad][pad][pad]
```

Three bytes of padding after `a` to align `b`, then three more at the end
so that an array of `Padded` keeps every element aligned. Total 12.

```
struct Packedish { int b; char a; char c; };

byte:  0    1    2    3    4    5    6    7
      [   b (4 bytes)   ] [a] [c] [pad][pad]
```

`b` is already aligned at offset 0, the two chars pack together, two bytes
of tail padding. Total 8.

**The rule:** declare members largest-first and padding mostly disappears.
This matters when you have a million of something. It does not matter for
a struct you make three of, so don't contort your code for it.

You can check your own types:

```cpp
static_assert(sizeof(MyStruct) == 16, "unexpected layout");
std::printf("%zu\n", offsetof(MyStruct, member));
```

---

## where this shows up in agentty

`enum class Role : std::uint8_t` — the `: std::uint8_t` is a size promise.
Without it the enum is `int`-sized, and a `Role` that gets written into a
thread file would take 4 bytes instead of 1. With thousands of messages,
that adds up, and more importantly the on-disk format would change if a
compiler picked a different underlying type.

`std::uint32_t width` — an image is never 5 billion pixels wide, and it's
never −40 pixels wide. The type says both.

---

## check yourself

Answer before you run anything:

1. What does `sizeof(char)` return, on every platform, guaranteed?
2. Why is `*reinterpret_cast<float*>(&some_int)` wrong when `memcpy` is
   right?
3. `struct S { double d; char c; };` — what's `sizeof(S)` and why?
4. `int total = 7; int done = 5;` — write the percentage two ways.
5. Why does agentty write `enum class Role : std::uint8_t` and not just
   `enum class Role`?

<details>
<summary>answers</summary>

1. `1`. Always. `char` is the unit `sizeof` counts in, by definition.
2. Strict aliasing: the compiler assumes a `float*` and an `int*` never
   point at the same bytes, so it's free to reorder or cache loads across
   them. `memcpy` has an explicit exception carved out for it.
3. 16. The `double` takes 8, the `char` takes 1, then 7 bytes of tail
   padding so arrays stay 8-aligned.
4. `(done * 100) / total` or `(static_cast<double>(done) / total) * 100.0`.
5. To pin the size and the on-disk representation. Without it the
   underlying type is implementation-defined (usually `int`).

</details>

---

[next: integers lie to you →](02-integers-lie.md)
