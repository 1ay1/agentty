[← the model](00-the-model.md) · [chapter index](README.md) · [next: integers lie →](02-integers-lie.md)

# 1. What a type actually is

**Time:** 45 minutes. Type everything.

Open an empty file called `01_types.cpp`. We're going to build it
together, and by the end you'll have written a program that answers three
questions about types. Don't copy-paste. Type it.

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

Let's prove each one.

---

## question 1: how many bytes

Start your file with the includes we'll need and the first function:

```cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static void sizes() {
    std::puts("-- sizes on this machine --");
    std::printf("char        %2zu byte   align %zu\n", sizeof(char), alignof(char));
    std::printf("short       %2zu bytes  align %zu\n", sizeof(short), alignof(short));
    std::printf("int         %2zu bytes  align %zu\n", sizeof(int), alignof(int));
    std::printf("long        %2zu bytes  align %zu\n", sizeof(long), alignof(long));
    std::printf("float       %2zu bytes  align %zu\n", sizeof(float), alignof(float));
    std::printf("double      %2zu bytes  align %zu\n", sizeof(double), alignof(double));
    std::printf("void*       %2zu bytes  align %zu\n", sizeof(void*), alignof(void*));
    std::printf("std::string %2zu bytes  align %zu\n", sizeof(std::string), alignof(std::string));
}
```

A few things to notice in what you just typed:

- **`static`** on a free function means "only visible in this file". It's
  the C++ way of saying private at file scope. Use it for helpers that
  aren't part of an interface.
- **`sizeof`** is an operator, not a function, and it runs at *compile
  time*. The number is baked into the binary. It yields a `std::size_t`.
- **`alignof`** gives the alignment requirement: the address of an object
  of this type must be divisible by that number.
- **`%zu`** is the printf format for `size_t`. Using `%d` there is
  undefined behaviour on 64-bit — the argument is 8 bytes and `%d` reads
  4. Get this right; it's a real bug, not a style point.
- **`std::puts`** vs **`std::printf`**: `puts` appends a newline, `printf`
  doesn't. That's why the `printf` lines end in `\n` and the `puts` line
  doesn't.

Now add a `main` so you can run it:

```cpp
int main() {
    sizes();
}
```

Compile and run:

```sh
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g \
    01_types.cpp -o 01_types && ./01_types
```

You should get something like:

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

### what's guaranteed and what isn't

Only two things in that output are promised by the standard.
`sizeof(char)` is **always** 1 — it's the unit `sizeof` counts in, by
definition. And the ordering
`char ≤ short ≤ int ≤ long ≤ long long` always holds.

Everything else is your compiler and platform. On a 32-bit build,
`void*` would be 4. On Windows, `long` is 4 even on 64-bit.

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
```

The `: std::uint8_t` is a size promise. Without it the enum's underlying
type is implementation-defined (usually `int`), so a `Role` written into a
thread file would take 4 bytes instead of 1 — and worse, the on-disk
format could change if someone built with a different compiler.

### why `std::string` is 32 bytes

Worth a moment, because it explains a lot of C++ performance.

libstdc++ lays `std::string` out as roughly:

```cpp
struct string {
    char*  ptr;                 // 8 bytes
    size_t size;                // 8 bytes
    union {
        char   buf[16];         // 16 bytes
        size_t capacity;
    };
};                              // 32 total
```

A string of 15 characters or fewer lives **entirely inside that 16-byte
buffer**, with no heap allocation at all. That's the *small string
optimisation* (SSO), and it's why passing strings around in C++ is cheaper
than it looks. It's also why the moved-from demos later use deliberately
long strings — a short one might not have a heap buffer to steal.

---

## question 2: how to read the bytes

Add this function above `main`:

```cpp
static void reinterpretation() {
    std::puts("\n-- same 4 bytes, two types --");
    std::uint32_t bits = 0x41200000u;     // this is 10.0f as IEEE-754
    float f;
    std::memcpy(&f, &bits, sizeof f);     // the legal way to type-pun
    std::printf("as uint32_t: %u\n", bits);
    std::printf("as float   : %.1f\n", static_cast<double>(f));
    std::puts("the bytes never changed. only the type we read them with did.");
}
```

and call it from `main`:

```cpp
int main() {
    sizes();
    reinterpretation();
}
```

```
-- same 4 bytes, two types --
as uint32_t: 1092616192
as float   : 10.0
the bytes never changed. only the type we read them with did.
```

`0x41200000` is the integer 1092616192 and also the float 10.0. Same 32
bits. The type is the only thing that decides which.

### three details in that function

**Why `memcpy` and not a cast.** The obvious thing to write is:

```cpp
float f = *reinterpret_cast<float*>(&bits);   // WRONG. undefined behaviour.
```

That's a *strict aliasing* violation. The compiler is allowed to assume a
`float*` and a `std::uint32_t*` never point at the same memory, so it can
freely reorder or cache loads across them. Modern optimisers genuinely
break code that does this.

`memcpy` has an explicit carve-out in the standard for exactly this, and
every compiler turns a small fixed-size `memcpy` into a single move
instruction at `-O1` and up. It is not slower. In C++20 and later you can
also write:

```cpp
float f = std::bit_cast<float>(bits);   // same thing, nicer, constexpr-friendly
```

**Why `sizeof f` and not `sizeof(float)`.** `sizeof` on an expression
doesn't need parens, and using the variable means the two can never drift
apart if you change `f`'s type later. Small habit, prevents a real bug.

**Why the `static_cast<double>` in the printf.** `printf` is variadic, and
variadic arguments undergo *default argument promotions*: `float` is
promoted to `double` automatically. So `%f` actually expects a `double`.
Passing the float works either way, but the cast documents that you know
the promotion happens. Under `-Wall` you'd get no warning here, but being
explicit costs nothing.

---

## question 3: what you can do with it

Add:

```cpp
static void operations() {
    std::puts("\n-- / means different things --");
    int a = 7, b = 2;
    std::printf("7 / 2       = %d      (int division truncates)\n", a / b);
    std::printf("7.0 / 2.0   = %.1f\n", 7.0 / 2.0);
    std::printf("(double)7/2 = %.1f    (one cast fixes the whole expression)\n",
                static_cast<double>(a) / b);
}
```

```
-- / means different things --
7 / 2       = 3      (int division truncates)
7.0 / 2.0   = 3.5
(double)7/2 = 3.5    (one cast fixes the whole expression)
```

`/` is not one operation. On two ints it's integer division, which
truncates *toward zero*. On two doubles it's floating-point division. The
compiler picks based on the operand types.

Look at the third line carefully. We cast only `a`, not `b`. That's
enough: when one operand is `double` and the other is `int`, the `int` is
converted to `double` and the whole expression becomes floating-point.
These are the **usual arithmetic conversions**, and they'll come back in
section 2 to cause a much nastier bug.

### the bug this causes

Add this one, because it's the version you'll actually hit:

```cpp
static void the_progress_bug() {
    std::puts("\n-- a real percentage bug --");
    int done = 3, total = 4;
    std::printf("(done / total) * 100            = %d%%   WRONG\n",
                (done / total) * 100);
    std::printf("(done * 100) / total            = %d%%   ok for ints\n",
                (done * 100) / total);
    std::printf("(double(done) / total) * 100.0  = %.0f%%   ok, and reads right\n",
                (static_cast<double>(done) / total) * 100.0);
}
```

```
-- a real percentage bug --
(done / total) * 100            = 0%   WRONG
(done * 100) / total            = 75%   ok for ints
(double(done) / total) * 100.0  = 75%   ok, and reads right
```

`3 / 4` is `0`. Then `0 * 100` is `0`. Every progress bar written by
someone new to C++ has this bug once.

Two fixes, and they are **not** equivalent:

- `(done * 100) / total` stays in integer arithmetic. Correct and fast,
  but `done * 100` can overflow if `done` is large. Fine for a percentage
  where `done` is small, wrong for byte counts.
- `(double(done) / total) * 100.0` converts first. Correct for any
  magnitude, and it reads like what you meant.

Note `%%` in the format string — that's how you print a literal `%`.

---

## layout: order changes size

Last one. Add these two structs and a function, above `main`:

```cpp
struct Padded    { char a; int b; char c; };   // 1 + pad + 4 + 1 + pad
struct Packedish { int b; char a; char c; };   // 4 + 1 + 1 + pad

static void layout() {
    std::puts("\n-- member order changes the size --");
    std::printf("struct { char; int; char; } = %zu bytes\n", sizeof(Padded));
    std::printf("struct { int; char; char; } = %zu bytes\n", sizeof(Packedish));
    std::puts("same three members. the compiler pads to keep int aligned.");
}
```

```
-- member order changes the size --
struct { char; int; char; } = 12 bytes
struct { int; char; char; } = 8 bytes
same three members. the compiler pads to keep int aligned.
```

Identical members. Only the order changed, and the struct got 33% smaller.

Here's exactly why. An `int` must sit at an address divisible by 4.

```
struct Padded { char a; int b; char c; };

byte:  0    1    2    3    4    5    6    7    8    9   10   11
      [a] [pad][pad][pad][   b (4 bytes)   ] [c] [pad][pad][pad]
       ^                  ^                  ^
       offset 0           offset 4           offset 8
                          (divisible by 4)
```

Three bytes of padding after `a` so `b` lands on a multiple of 4. Then
three more bytes at the *end* — tail padding — so that in an array of
`Padded`, element 1 starts at offset 12, which is still 4-aligned. Total
12.

```
struct Packedish { int b; char a; char c; };

byte:  0    1    2    3    4    5    6    7
      [   b (4 bytes)   ] [a] [c] [pad][pad]
```

`b` is already aligned at offset 0. The two chars pack together at 4 and
5. Two bytes of tail padding to keep the struct 4-aligned. Total 8.

**The rule:** declare members largest-first and padding mostly disappears.

**The caveat:** this matters when you have a million of something. It does
not matter for a struct you make three of. Don't contort readable code for
four bytes you'll never notice.

You can check your own types:

```cpp
static_assert(sizeof(MyStruct) == 16, "unexpected layout");
std::printf("%zu\n", offsetof(MyStruct, member));   // needs <cstddef>
```

A `static_assert` like that in a header is a good way to pin a wire format
so nobody breaks it by adding a field.

---

## the whole file

Your `01_types.cpp` should now look like this. Compare against yours — if
something differs, the difference is worth understanding.

```cpp
// 01_types.cpp — what a type actually is: size, layout, and operations.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// ── a type is a promise about bytes ────────────────────────────────────
static void sizes() {
    std::puts("-- sizes on this machine --");
    std::printf("char        %2zu byte   align %zu\n", sizeof(char), alignof(char));
    std::printf("short       %2zu bytes  align %zu\n", sizeof(short), alignof(short));
    std::printf("int         %2zu bytes  align %zu\n", sizeof(int), alignof(int));
    std::printf("long        %2zu bytes  align %zu\n", sizeof(long), alignof(long));
    std::printf("float       %2zu bytes  align %zu\n", sizeof(float), alignof(float));
    std::printf("double      %2zu bytes  align %zu\n", sizeof(double), alignof(double));
    std::printf("void*       %2zu bytes  align %zu\n", sizeof(void*), alignof(void*));
    std::printf("std::string %2zu bytes  align %zu\n", sizeof(std::string), alignof(std::string));
}

// ── the same bytes, read as two different types ────────────────────────
static void reinterpretation() {
    std::puts("\n-- same 4 bytes, two types --");
    std::uint32_t bits = 0x41200000u;     // this is 10.0f as IEEE-754
    float f;
    std::memcpy(&f, &bits, sizeof f);     // the legal way to type-pun
    std::printf("as uint32_t: %u\n", bits);
    std::printf("as float   : %.1f\n", static_cast<double>(f));
    std::puts("the bytes never changed. only the type we read them with did.");
}

// ── operations come from the type, not the value ───────────────────────
static void operations() {
    std::puts("\n-- / means different things --");
    int a = 7, b = 2;
    std::printf("7 / 2       = %d      (int division truncates)\n", a / b);
    std::printf("7.0 / 2.0   = %.1f\n", 7.0 / 2.0);
    std::printf("(double)7/2 = %.1f    (one cast fixes the whole expression)\n",
                static_cast<double>(a) / b);
}

// ── the bug this causes in real code ───────────────────────────────────
static void the_progress_bug() {
    std::puts("\n-- a real percentage bug --");
    int done = 3, total = 4;
    std::printf("(done / total) * 100            = %d%%   WRONG\n",
                (done / total) * 100);
    std::printf("(done * 100) / total            = %d%%   ok for ints\n",
                (done * 100) / total);
    std::printf("(double(done) / total) * 100.0  = %.0f%%   ok, and reads right\n",
                (static_cast<double>(done) / total) * 100.0);
}

// ── layout: a struct is its members, in order, with padding ────────────
struct Padded    { char a; int b; char c; };   // 1 + pad + 4 + 1 + pad
struct Packedish { int b; char a; char c; };   // 4 + 1 + 1 + pad

static void layout() {
    std::puts("\n-- member order changes the size --");
    std::printf("struct { char; int; char; } = %zu bytes\n", sizeof(Padded));
    std::printf("struct { int; char; char; } = %zu bytes\n", sizeof(Packedish));
    std::puts("same three members. the compiler pads to keep int aligned.");
}

int main() {
    sizes();
    reinterpretation();
    operations();
    the_progress_bug();
    layout();
}
```

---

## now break it

Reading is not learning. Do at least three of these to your own file:

1. Change `Padded` to `struct { char a; char c; int b; };`. Predict the
   size before you compile.
2. Add a `double` to `Packedish`. Predict again.
3. Replace the `memcpy` with `*reinterpret_cast<float*>(&bits)`. It'll
   probably still work at `-O0`. Now compile with `-O2` and
   `-fstrict-aliasing` and see if it still does. (It might! UB isn't
   guaranteed to fail, which is what makes it dangerous.)
4. Change `%zu` to `%d` in one of the `sizes()` lines. Read the warning
   `-Wformat` gives you.
5. Work out what `0x40490FDB` is as a float before running it.

---

## check yourself

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
3. 16. The `double` takes 8 at offset 0, the `char` takes 1 at offset 8,
   then 7 bytes of tail padding so arrays stay 8-aligned.
4. `(done * 100) / total` or `(static_cast<double>(done) / total) * 100.0`.
5. To pin the size and the on-disk representation. Without it the
   underlying type is implementation-defined, so the serialised form could
   change between compilers.

</details>

---

[next: integers lie to you →](02-integers-lie.md)
