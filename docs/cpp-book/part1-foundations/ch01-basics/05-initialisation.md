[← value categories](04-value-categories.md) · [chapter index](README.md) · [next: references and const →](06-references-and-const.md)

# 5. Initialisation

**Time:** 45 minutes
**Code:** [`code/05_init.cpp`](code/05_init.cpp)

```sh
cd code && make 05_init && ./05_init
```

C++ has five initialisation syntaxes and they are not interchangeable.
Here's which to use and the one case where the recommended one surprises
you.

---

## the five

```cpp
int a;           // 1. default     -- INDETERMINATE for a local. do not read.
int b = 5;       // 2. copy
int c(6);        // 3. direct
int d{7};        // 4. list (braced)
int e = {8};     // 5. copy-list
```

```
-- five forms --
b=5 c=6 d=7 e=8   (a is left unread on purpose)
```

### form 1 is a trap for built-ins

```cpp
int a;              // indeterminate value. reading it is UB.
std::string s;      // fine: string's default ctor runs, s is ""
```

For a class type, default-init calls the default constructor and you get a
well-defined object. For a built-in local, you get whatever was in that
stack slot. Reading it is undefined behaviour, and it's the kind that
works in debug and fails in release.

Write `int a{};` (zero) or `int a = compute();` instead. There is
essentially never a reason to leave a local built-in uninitialised.

(Globals and `static` locals are an exception — they're zero-initialised
before anything else happens. But relying on that is a bad habit.)

---

## why braces

```
-- braces reject narrowing --
uint8_t ok{255}  = 255
uint8_t bad{256};      -> COMPILE ERROR (narrowing)
uint8_t bad = 256;     -> compiles, gives you 0
int i{3.5};            -> COMPILE ERROR
int i = 3.5;           -> compiles, gives you 3
that difference is the whole argument for {}.
```

Braced init forbids **narrowing conversions**: any conversion that could
lose information. `256` doesn't fit in a `uint8_t`, `3.5` isn't an
integer, so both are errors.

With `=` they compile silently and give you a wrong number. Section 2
covered why that's bad.

Braces also fix the *most vexing parse*:

```
-- the most vexing parse --
std::string s();   declares a FUNCTION returning string.
std::string s{};   declares a string. braces are unambiguous.
```

`std::string s();` does not make an empty string. It declares a function
named `s` taking nothing and returning `std::string`. You get a confusing
error later when you try to use `s` as a string.

The rule the compiler follows: **if it could be a declaration, it is a
declaration.** Parentheses are ambiguous; braces can't start a parameter
list, so they aren't.

The worst version:

```cpp
Widget w(Gadget(), Thing());   // a function taking two function pointers
Widget w{Gadget{}, Thing{}};   // a Widget. what you meant.
```

---

## the one place braces surprise you

```
-- the one place braces surprise you --
vector<int> v(3, 7) -> size 3: 7 7 7
vector<int> v{3, 7} -> size 2: 3 7
initializer_list always wins when one is viable.
so: braces by default, parens when you mean a count.
```

`std::vector` has a constructor taking `std::initializer_list<int>` and
one taking `(size_type count, const int& value)`. When you write braces
and an `initializer_list` constructor is *viable at all*, it wins.
Unconditionally. Even if another constructor is a better match.

So:

- `v(3, 7)` — three sevens
- `v{3, 7}` — the two elements 3 and 7

This catches everyone once. It also applies to `std::string`:

```cpp
std::string a(3, 'x');   // "xxx"
std::string b{3, 'x'};   // two chars: '\3' and 'x'. not what you want.
```

**The rule that works:** braces by default. Parentheses when you're
passing a *count* or otherwise deliberately calling a specific
constructor.

---

## aggregates and designated initialisers

```
-- aggregates --
Point p{3, 4}          -> x=3 y=4
Point q{.x=10, .y=20}  -> x=10 y=20
Point z{}              -> x=0 y=0  (zeroed)
```

An **aggregate** is a class with no user-declared constructors, no private
non-static data, no virtuals, no base classes with those things. Plain
data. You initialise it by listing its members.

```cpp
struct Point { int x; int y; };
Point p{3, 4};
```

C++20 added **designated initialisers**, where you name the fields:

```cpp
Point q{.x = 10, .y = 20};
```

agentty uses these where it matters for readability. From section 10's
capstone, and from the real `LazyBytes`:

```cpp
ImageContent::Source{.blob = std::move(name), .b64 = {}}
```

Without the names you'd write `Source{name, {}}` and the reader has to go
look up which field is which. With them, the call site documents itself.

Two rules C++ imposes that C doesn't:

1. **Order must match declaration order.** `Point{.y = 1, .x = 2}` is an
   error. C allows it, C++ doesn't.
2. **You can skip fields**, and skipped ones get their default member
   initialiser, or value-init if there isn't one.

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
body *default-constructs it first, then assigns over it* — two operations
where one would do, and for a `const` member or a reference member it
doesn't compile at all.

```cpp
// bad
Message(std::string t) { text = t; }        // default-ctor then copy-assign

// good
Message(std::string t) : text(std::move(t)) {}   // one move-construct
```

**Members are initialised in declaration order, not init-list order.**
Write them in a different order in the list and `-Wreorder` will tell you.
Listen to it — if one member's initialiser reads another, the order is
load-bearing.

---

## what to actually do

- `T x{};` when you want a default. Never `T x;` for a built-in local.
- `T x{value};` for a normal init.
- `T x(count, value);` when a constructor takes a count.
- `auto x = expr;` when the type is obvious from the right-hand side.
- Designated initialisers for aggregates with more than two fields.
- Default member initialisers for anything with an obvious default.

---

## check yourself

1. `int x;` inside a function — what's its value?
2. `std::vector<int> v{5};` versus `std::vector<int> v(5);` — what's in
   each?
3. What does `std::string s();` declare?
4. Why does `std::uint8_t b{300}` fail but `std::uint8_t b = 300` compile?
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
