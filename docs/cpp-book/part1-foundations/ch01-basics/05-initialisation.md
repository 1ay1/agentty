[← value categories](04-value-categories.md) · [chapter index](README.md) · [next: references and const →](06-references-and-const.md)

# 5. Initialisation

**Time:** 45 minutes. Type everything.

C++ has five initialisation syntaxes and they are not interchangeable.
Here's which to use, and the one case where the recommended one surprises
you.

Open `05_init.cpp`.

---

## the five

```cpp
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static void the_five_forms() {
    std::puts("-- five forms --");
    int a;           // 1. default: INDETERMINATE for a local int. never read it.
    int b = 5;       // 2. copy
    int c(6);        // 3. direct
    int d{7};        // 4. list (braced) -- prefer this
    int e = {8};     // 5. copy-list
    (void)a;
    std::printf("b=%d c=%d d=%d e=%d   (a is left unread on purpose)\n",
                b, c, d, e);
}

int main() {
    the_five_forms();
}
```

```
-- five forms --
b=5 c=6 d=7 e=8   (a is left unread on purpose)
```

Note the `(void)a;`. We declared `a` to show form 1 exists, then
deliberately never read it, because reading it is undefined behaviour. The
cast just silences `-Wunused-variable` without touching the value.

### form 1 is a trap for built-ins

```cpp
int a;              // indeterminate value. reading it is UB.
std::string s;      // fine: string's default ctor runs, s is ""
```

For a **class type**, default-init calls the default constructor and you
get a well-defined object. For a **built-in local**, you get whatever was
in that stack slot. Reading it is UB, and it's the kind that works in
debug and fails in release.

Write `int a{};` (zero) or `int a = compute();` instead. There is
essentially never a reason to leave a local built-in uninitialised.

> Globals and `static` locals are an exception — they're zero-initialised
> before anything else happens. But relying on that is a bad habit that
> stops working the moment the variable becomes a local.

---

## why braces

```cpp
static void braces_catch_narrowing() {
    std::puts("\n-- braces reject narrowing --");
    std::uint8_t ok{255};
    std::printf("uint8_t ok{255}  = %u\n", static_cast<unsigned>(ok));
    std::puts("uint8_t bad{256};      -> COMPILE ERROR (narrowing)");
    std::puts("uint8_t bad = 256;     -> compiles, gives you 0");
    std::puts("int i{3.5};            -> COMPILE ERROR");
    std::puts("int i = 3.5;           -> compiles, gives you 3");
}
```

Don't take the strings' word for it. **Actually write the failing lines**
and compile:

```cpp
std::uint8_t bad{256};
```

```
error: narrowing conversion of '256' from 'int' to 'std::uint8_t'
{aka 'unsigned char'} [-Wnarrowing]
```

Braced init forbids **narrowing conversions**: any conversion that could
lose information. `256` doesn't fit in a `uint8_t`, `3.5` isn't an
integer, so both are errors.

With `=` they compile silently and give you a wrong number. Section 2
covered why that's bad.

---

## the most vexing parse

```cpp
static void most_vexing_parse() {
    std::puts("\n-- the most vexing parse --");
    std::puts("std::string s();   declares a FUNCTION returning string.");
    std::puts("std::string s{};   declares a string. braces are unambiguous.");
    std::string s{};
    std::printf("s.empty() = %s\n", s.empty() ? "true" : "false");
}
```

Write the bad version and try to use it:

```cpp
std::string s();
std::printf("%zu\n", s.size());
```

```
error: request for member 'size' in 's', which is of non-class type
'std::string()'
```

`std::string s();` does **not** make an empty string. It declares a
function named `s`, taking nothing, returning `std::string`. You get a
confusing error later when you try to use `s` as a string.

The rule the compiler follows: **if it could be a declaration, it is a
declaration.** Parentheses are ambiguous — they could start a parameter
list. Braces can't, so they aren't.

The worst version:

```cpp
Widget w(Gadget(), Thing());   // a FUNCTION taking two function pointers
Widget w{Gadget{}, Thing{}};   // a Widget. what you meant.
```

---

## the one place braces surprise you

```cpp
static void the_vector_gotcha() {
    std::puts("\n-- the one place braces surprise you --");
    std::vector<int> paren(3, 7);    // three elements, each 7
    std::vector<int> brace{3, 7};    // two elements: 3 and 7

    std::printf("vector<int> v(3, 7) -> size %zu: ", paren.size());
    for (int v : paren) std::printf("%d ", v);
    std::putchar('\n');

    std::printf("vector<int> v{3, 7} -> size %zu: ", brace.size());
    for (int v : brace) std::printf("%d ", v);
    std::putchar('\n');
}
```

```
-- the one place braces surprise you --
vector<int> v(3, 7) -> size 3: 7 7 7
vector<int> v{3, 7} -> size 2: 3 7
```

`std::vector` has a constructor taking `std::initializer_list<int>` and
one taking `(size_type count, const int& value)`. When you write braces
and an `initializer_list` constructor is **viable at all**, it wins.
Unconditionally. Even if another constructor is a better match.

So:

- `v(3, 7)` — three sevens
- `v{3, 7}` — the two elements 3 and 7

This catches everyone once. It applies to `std::string` too:

```cpp
std::string a(3, 'x');   // "xxx"
std::string b{3, 'x'};   // two chars: '\3' and 'x'. not what you want.
```

Try printing both. The second one prints a control character and an `x`.

**The rule that works:** braces by default. Parentheses when you're
passing a *count* or otherwise deliberately calling a specific
constructor.

---

## aggregates and designated initialisers

```cpp
struct Point { int x; int y; };

static void aggregates_and_designators() {
    std::puts("\n-- aggregates --");
    Point p{3, 4};
    std::printf("Point p{3, 4}          -> x=%d y=%d\n", p.x, p.y);

    Point q{.x = 10, .y = 20};        // C++20 designated initialisers
    std::printf("Point q{.x=10, .y=20}  -> x=%d y=%d\n", q.x, q.y);

    Point z{};                        // value-init: all members zeroed
    std::printf("Point z{}              -> x=%d y=%d  (zeroed)\n", z.x, z.y);
}
```

```
-- aggregates --
Point p{3, 4}          -> x=3 y=4
Point q{.x=10, .y=20}  -> x=10 y=20
Point z{}              -> x=0 y=0  (zeroed)
```

An **aggregate** is a class with no user-declared constructors, no private
non-static data, no virtuals, no base classes with those things. Plain
data. You initialise it by listing its members.

### why designated initialisers matter

agentty uses these where readability wins. From `LazyBytes`:

```cpp
Source{.blob = std::move(name), .b64 = {}}
```

Without the names you'd write `Source{name, {}}` and the reader has to go
look up which field is which. With them, the call site documents itself.

Two rules C++ imposes that C doesn't:

**1. Order must match declaration order.** Try it:

```cpp
Point bad{.y = 1, .x = 2};
```

```
error: designator order for field 'Point::x' does not match declaration
order in 'Point'
```

C allows any order; C++ doesn't.

**2. You can skip fields**, and skipped ones get their default member
initialiser, or value-init if there isn't one:

```cpp
struct Config { int retries = 3; bool verbose = false; std::string host; };
Config c{.verbose = true};     // retries stays 3, host is ""
```

### `{}` means "zero it"

```cpp
Point z{};          // both members zero-initialised
int   n{};          // 0
std::string s{};    // ""
double* p{};        // nullptr
```

`T x{}` is **value-initialisation** and it's the safe default for
anything. For an aggregate it zeroes every member. For a class with a
default constructor it calls it. It never leaves you with garbage.

---

## member initialisation, three ways

```cpp
struct Message {
    Role        role = Role::User;        // 1. default member initialiser
    std::string text;
    MessageId   id;

    Message() = default;

    explicit Message(std::string t)
        : text(std::move(t)),             // 2. member init list
          id(new_message_id())
    {
        // 3. assignment in the body -- usually wrong
    }
};
```

**Prefer 1 and 2. Avoid 3.**

The member init list constructs the member directly. Assignment in the
body **default-constructs it first, then assigns over it** — two
operations where one would do:

```cpp
// bad
Message(std::string t) { text = t; }             // default-ctor then copy-assign

// good
Message(std::string t) : text(std::move(t)) {}   // one move-construct
```

And for a `const` member or a reference member, assignment in the body
doesn't compile at all — there's nothing to assign to.

### the ordering trap

**Members are initialised in declaration order, not init-list order.**

```cpp
struct Bad {
    int b;
    int a;
    Bad() : a(1), b(a) {}   // b is initialised FIRST, from an uninitialised a
};
```

```
warning: 'Bad::a' will be initialized after [-Wreorder]
warning:   'int Bad::b' [-Wreorder]
```

Read it as: "you wrote `a` first in the init list, but `a` will actually
be initialised *after* `b`." Since `b(a)` reads `a`, and `b` runs first,
`b` gets garbage.

Listen to `-Wreorder`. If one member's initialiser reads another, the
declaration order is load-bearing and getting it wrong reads garbage.

---

## the whole file

```cpp
// 05_init.cpp — the five ways to initialise, and which to use.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Point { int x; int y; };

static void the_five_forms() {
    std::puts("-- five forms --");
    int a;           // 1. default: INDETERMINATE for a local int. never read it.
    int b = 5;       // 2. copy
    int c(6);        // 3. direct
    int d{7};        // 4. list (braced) -- prefer this
    int e = {8};     // 5. copy-list
    (void)a;
    std::printf("b=%d c=%d d=%d e=%d   (a is left unread on purpose)\n",
                b, c, d, e);
}

static void braces_catch_narrowing() {
    std::puts("\n-- braces reject narrowing --");
    std::uint8_t ok{255};
    std::printf("uint8_t ok{255}  = %u\n", static_cast<unsigned>(ok));
    std::puts("uint8_t bad{256};      -> COMPILE ERROR (narrowing)");
    std::puts("uint8_t bad = 256;     -> compiles, gives you 0");
    std::puts("int i{3.5};            -> COMPILE ERROR");
    std::puts("int i = 3.5;           -> compiles, gives you 3");
    std::puts("that difference is the whole argument for {}.");
}

static void the_vector_gotcha() {
    std::puts("\n-- the one place braces surprise you --");
    std::vector<int> paren(3, 7);    // three elements, each 7
    std::vector<int> brace{3, 7};    // two elements: 3 and 7

    std::printf("vector<int> v(3, 7) -> size %zu: ", paren.size());
    for (int v : paren) std::printf("%d ", v);
    std::putchar('\n');

    std::printf("vector<int> v{3, 7} -> size %zu: ", brace.size());
    for (int v : brace) std::printf("%d ", v);
    std::putchar('\n');
    std::puts("initializer_list always wins when one is viable.");
    std::puts("so: braces by default, parens when you mean a count.");
}

static void most_vexing_parse() {
    std::puts("\n-- the most vexing parse --");
    std::puts("std::string s();   declares a FUNCTION returning string.");
    std::puts("std::string s{};   declares a string. braces are unambiguous.");
    std::string s{};
    std::printf("s.empty() = %s\n", s.empty() ? "true" : "false");
}

static void aggregates_and_designators() {
    std::puts("\n-- aggregates --");
    Point p{3, 4};
    std::printf("Point p{3, 4}          -> x=%d y=%d\n", p.x, p.y);

    Point q{.x = 10, .y = 20};        // C++20 designated initialisers
    std::printf("Point q{.x=10, .y=20}  -> x=%d y=%d\n", q.x, q.y);
    std::puts("designators are how agentty writes LazyBytes::Source:");
    std::puts("  Source{.blob = std::move(name), .b64 = {}}");
    std::puts("order must match declaration order. you cannot skip around.");

    Point z{};                        // value-init: all members zeroed
    std::printf("Point z{}              -> x=%d y=%d  (zeroed)\n", z.x, z.y);
}

int main() {
    the_five_forms();
    braces_catch_narrowing();
    the_vector_gotcha();
    most_vexing_parse();
    aggregates_and_designators();
}
```

---

## what to actually do

- `T x{};` when you want a default. Never `T x;` for a built-in local.
- `T x{value};` for a normal init.
- `T x(count, value);` when a constructor takes a count.
- `auto x = expr;` when the type is obvious from the right-hand side.
- Designated initialisers for aggregates with more than two fields.
- Default member initialisers for anything with an obvious default.

---

## now break it

1. Write `std::uint8_t bad{256};` and read the error. Then `= 256` and
   print it.
2. Write `std::string s(); s.size();` and read that error too.
3. Print `std::string b{3, 'x'}` character by character with `%d` and
   work out what you got.
4. Write `Point bad{.y = 1, .x = 2};` and read the ordering error.
5. Write the `struct Bad` reorder example and compile with
   `-Wall -Wextra`. Then run it and see what `b` actually contains.
6. Add a `const int` member to `Point` and try to assign it in a
   constructor body.

---

## check yourself

1. `int x;` inside a function — what's its value?
2. `std::vector<int> v{5};` versus `std::vector<int> v(5);` — what's in
   each?
3. What does `std::string s();` declare?
4. Why does `std::uint8_t b{300}` fail but `std::uint8_t b = 300`
   compile?
5. `struct P { int x; int y; };` — is `P p{.y = 1, .x = 2};` legal C++?

<details>
<summary>answers</summary>

1. Indeterminate. Reading it is undefined behaviour.
2. `v{5}` is one element with value 5. `v(5)` is five elements, each 0.
3. A function named `s`, taking no parameters, returning `std::string`.
   The most vexing parse.
4. Braced init forbids narrowing conversions; `=` init allows them
   silently.
5. No. Designated initialisers must follow declaration order in C++
   (C allows any order).

</details>

---

[next: references and const →](06-references-and-const.md)
