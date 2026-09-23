[← lifetime](07-lifetime.md) · [chapter index](README.md) · [next: auto and decltype →](09-auto-and-decltype.md)

# 8. Copy, move, elision

**Time:** 75 minutes. Type everything.

This section is where you stop guessing about performance. We'll build a
type that narrates its own life, then use it to answer questions you
probably can't answer confidently right now.

Open `08_copy_move.cpp`.

---

## first, build the instrument

Every claim in this section is measured with this one type. Type it
carefully — the printouts are what make everything else visible.

```cpp
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct Tracked {
    std::string name;

    explicit Tracked(std::string n) : name(std::move(n)) {
        std::printf("  ctor      %s\n", name.c_str());
    }
    Tracked(const Tracked& o) : name(o.name) {
        std::printf("  copy-ctor %s\n", name.c_str());
    }
    Tracked(Tracked&& o) noexcept : name(std::move(o.name)) {
        std::printf("  move-ctor %s\n", name.c_str());
    }
    Tracked& operator=(const Tracked& o) {
        name = o.name;
        std::printf("  copy-asgn %s\n", name.c_str());
        return *this;
    }
    Tracked& operator=(Tracked&& o) noexcept {
        name = std::move(o.name);
        std::printf("  move-asgn %s\n", name.c_str());
        return *this;
    }
    ~Tracked() {
        std::printf("  dtor      %s\n", name.empty() ? "(moved-from)" : name.c_str());
    }
};
```

That's all five special members plus a real constructor. Some details
worth noticing in what you typed:

- **The move constructor takes `Tracked&&` and then uses
  `std::move(o.name)`.** Yes, even though `o` is declared `Tracked&&`.
  Section 4 explained why: `o` is a *name*, so the expression `o` is an
  lvalue. Without the `std::move`, `name(o.name)` would **copy** the
  string, and your "move constructor" would silently be a copy.
- **`noexcept` on both move operations.** This is load-bearing and we'll
  measure it below.
- **The copy assignment returns `*this` by reference.** That's the
  convention so `a = b = c` chains work.
- **The destructor prints `(moved-from)` when the name is empty**, which
  is how you'll spot husks in the output.

---

## copy versus move

```cpp
static void copy_vs_move() {
    std::puts("-- copy vs move --");
    Tracked a{"A"};
    std::puts(" Tracked b = a;");
    Tracked b = a;
    std::puts(" Tracked c = std::move(a);");
    Tracked c = std::move(a);
    std::puts(" scope end:");
}

int main() {
    copy_vs_move();
}
```

```
-- copy vs move --
  ctor      A
 Tracked b = a;
  copy-ctor A
 Tracked c = std::move(a);
  move-ctor A
 scope end:
  dtor      A
  dtor      A
  dtor      (moved-from)
```

**Copy** duplicates. Both objects are independent, both own their
resources. Costs whatever the resource costs — for a `std::string` holding
a megabyte, that's a megabyte of allocation plus a memcpy.

**Move** transfers. The source gives up ownership, the destination takes
it. For a `std::string` that's three pointer writes, regardless of size.

Look at the last three lines. Destruction is reverse of construction
(`c`, `b`, `a`), and `a` reports `(moved-from)` because `c` took its
buffer.

---

## elision: the copy that never happens

Now the part that surprises people. Add three functions:

```cpp
static Tracked make_rvo() {
    return Tracked{"RVO"};           // C++17: guaranteed, no move at all
}

static Tracked make_nrvo() {
    Tracked local{"NRVO"};
    return local;                    // named: elision allowed, not guaranteed
}

static Tracked make_pessimised() {
    Tracked local{"pessimised"};
    return std::move(local);         // NEVER do this. it BLOCKS elision.
}

static void elision() {
    std::puts("\n-- elision --");
    std::puts(" auto r = make_rvo();");
    auto r = make_rvo();
    std::puts(" auto n = make_nrvo();");
    auto n = make_nrvo();
    std::puts(" auto p = make_pessimised();   <- extra move appears");
    auto p = make_pessimised();
    std::puts(" scope end:");
    (void)r; (void)n; (void)p;
}
```

Before you run it, **predict the output.** How many operations does each
of the three cost? Write it down.

```
-- elision --
 auto r = make_rvo();
  ctor      RVO
 auto n = make_nrvo();
  ctor      NRVO
 auto p = make_pessimised();   <- extra move appears
  ctor      pessimised
  move-ctor pessimised
  dtor      (moved-from)
```

### RVO: guaranteed since C++17

```cpp
return Tracked{"RVO"};
```

**One `ctor`. That's the whole cost.** No copy, no move, not even an
elided one.

In C++17 this isn't an optimisation, it's a *language rule*. A prvalue
returned from a function is not a temporary that gets copied — the object
is constructed **directly into the caller's storage**. There is only ever
one object in existence.

It works even for types with **deleted** copy and move constructors,
which is the proof that no copy or move is conceptually involved:

```cpp
struct NoCopyNoMove {
    NoCopyNoMove() = default;
    NoCopyNoMove(const NoCopyNoMove&) = delete;
    NoCopyNoMove(NoCopyNoMove&&) = delete;
};
NoCopyNoMove make() { return NoCopyNoMove{}; }   // compiles fine in C++17
auto x = make();                                  // so does this
```

Try that. It's a good way to convince yourself.

**So: return by value. Always.** It costs nothing.

### NRVO: allowed, and every compiler does it

```cpp
Tracked local{"NRVO"};
return local;
```

Also one `ctor` in practice. "Named RVO" applies when you return a named
local: the compiler constructs `local` directly in the caller's slot, so
the return is a no-op.

Unlike RVO, this is **permitted, not guaranteed**. The standard says the
compiler *may* elide; gcc, clang and msvc all do at `-O1` and above. And
even if one didn't, the fallback is a move — a named local in a return
statement is treated as an rvalue automatically — which is cheap.

### the pessimisation

```cpp
return std::move(local);      // NEVER
```

`ctor` + `move-ctor` + an extra `dtor`. **You made it strictly worse by
adding code.**

Here's the mechanism. Copy elision requires the returned expression to
*name* the local object. `std::move(local)` turns it from an lvalue naming
`local` into an xvalue, so the compiler can no longer identify it as the
object to construct in place. It must actually perform a move.

gcc catches this. Compile and look:

```
warning: moving a local object in a return statement prevents copy elision
[-Wpessimizing-move]
note: remove 'std::move' call
```

To keep it in the file as a demo without the warning breaking a `-Werror`
build, pragma it locally and say why:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpessimizing-move"
static Tracked make_pessimised() {
    Tracked local{"pessimised"};
    return std::move(local);
}
#pragma GCC diagnostic pop
```

**Rule: never `std::move` a return value.**

The one exception is returning a *member* of a local object, where
elision never applied in the first place:

```cpp
std::string take() { return std::move(member_); }   // this one is correct
```

---

## `noexcept` on a move constructor is not optional

This is the most valuable thing in the section. Add two types that are
identical except for one keyword:

```cpp
struct SafeMove {
    std::string s;
    explicit SafeMove(const char* c) : s(c) {}
    SafeMove(const SafeMove& o) : s(o.s) { std::puts("  COPY (safe)"); }
    SafeMove(SafeMove&& o) noexcept : s(std::move(o.s)) { std::puts("  move (safe)"); }
};

struct RiskyMove {
    std::string s;
    explicit RiskyMove(const char* c) : s(c) {}
    RiskyMove(const RiskyMove& o) : s(o.s) { std::puts("  COPY (risky)"); }
    RiskyMove(RiskyMove&& o) : s(std::move(o.s)) { std::puts("  move (risky)"); }
    // ^ no noexcept. vector will not trust it during reallocation.
};

static void noexcept_matters() {
    std::puts("\n-- why noexcept on a move ctor is not optional --");

    std::puts(" SafeMove, growing from 1 to 2:");
    std::vector<SafeMove> a;
    a.emplace_back("1");
    a.emplace_back("2");

    std::puts(" RiskyMove, growing from 1 to 2:");
    std::vector<RiskyMove> b;
    b.emplace_back("1");
    b.emplace_back("2");

    std::printf(" is_nothrow_move_constructible<SafeMove>  = %s\n",
                std::is_nothrow_move_constructible_v<SafeMove> ? "true" : "false");
    std::printf(" is_nothrow_move_constructible<RiskyMove> = %s\n",
                std::is_nothrow_move_constructible_v<RiskyMove> ? "true" : "false");
}
```

```
-- why noexcept on a move ctor is not optional --
 SafeMove, growing from 1 to 2:
  move (safe)
 RiskyMove, growing from 1 to 2:
  COPY (risky)
 is_nothrow_move_constructible<SafeMove>  = true
 is_nothrow_move_constructible<RiskyMove> = false
```

Two identical types. One keyword different. One gets **moved** on
reallocation, the other gets **copied**.

### why vector does this

`vector::push_back` offers the **strong exception guarantee**: if it
throws, the vector is unchanged.

Reallocation means allocate a new buffer, transfer N elements, free the
old one. Now suppose element 5 of 10 throws during the transfer:

- If we were **copying**, the originals are all still intact. Free the new
  buffer, rethrow, vector unchanged. Guarantee kept.
- If we were **moving**, elements 0–4 have been gutted and element 5 is
  half-built. We can't move them back, because those moves could throw
  too. The vector is destroyed. Guarantee broken.

So `vector` uses `std::move_if_noexcept`: move only when the move
constructor promises not to throw. Otherwise copy and stay safe.

### what this costs

A `vector<std::string>` with 10,000 strings, growing. With `noexcept`
moves: 10,000 pointer swaps. Without: 10,000 allocations and memcpys.
That's an order of magnitude, and it shows up as "why is my program slow"
with no obvious cause in a profile.

**Every move constructor and move assignment gets `noexcept`.** If yours
can't be, redesign until it can — usually that means holding a pointer or
a handle rather than something that allocates during the move.

Every move in agentty is `noexcept`, including `Id`'s constructor:

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

Pin it in a test so nobody breaks it:

```cpp
static_assert(std::is_nothrow_move_constructible_v<MyType>);
static_assert(std::is_nothrow_move_assignable_v<MyType>);
```

---

## the rule-of-five trap, measured

Now the bug this section exists for. Add this:

```cpp
struct ImplicitFine    { std::string s; };
struct ImplicitTrapped { std::string s; ~ImplicitTrapped() {} };
//                                      ^^^^^^^^^^^^^^^^^^^^ the only difference

static void the_trap() {
    std::puts("\n-- one empty destructor --");
    std::printf(" ImplicitFine    nothrow-movable? %s\n",
                std::is_nothrow_move_constructible_v<ImplicitFine> ? "yes" : "no");
    std::printf(" ImplicitTrapped nothrow-movable? %s\n",
                std::is_nothrow_move_constructible_v<ImplicitTrapped> ? "yes" : "no");
}
```

```
-- one empty destructor --
 ImplicitFine    nothrow-movable? yes
 ImplicitTrapped nothrow-movable? no
```

An **empty destructor**. Does nothing. And it flipped every future vector
reallocation from a pointer swap to a full deep copy.

Here's the chain, and it's worth being able to recite:

1. You declare `~ImplicitTrapped()`.
2. The compiler **stops generating the implicit move constructor and move
   assignment.** (The rule exists because a class needing a custom
   destructor probably manages a resource, and a default memberwise move
   would likely be wrong for it.)
3. `ImplicitTrapped b = std::move(a);` **still compiles** — overload
   resolution quietly falls back to the **copy** constructor.
4. The copy constructor can throw, so `is_nothrow_move_constructible_v`
   is false.
5. `vector` sees that and uses `move_if_noexcept`, which picks the copy.
6. Every reallocation now deep-copies every string. Forever.

No error. No warning. Not even at `-Wall -Wextra`. Your program is just
quietly slower, and the cause is one line that looks like it does nothing.

**And `= default` does not save you.** This is the part that catches
people who think they know the rule:

```cpp
struct A { std::string s; };                   // nothing declared
struct B { std::string s; ~B() {} };           // user-provided dtor
struct C { std::string s; ~C() = default; };   // user-DECLARED, defaulted

static_assert( std::is_nothrow_move_constructible_v<A>);   // passes
static_assert(!std::is_nothrow_move_constructible_v<B>);   // passes
static_assert(!std::is_nothrow_move_constructible_v<C>);   // ALSO passes
```

All three of those hold. It's *declaring* the destructor that suppresses
the implicit moves, not what's in its body. Writing `~C() = default;`
because it looked tidy costs you every move in the program.

The classic version is worse than slow — it's a crash:

```cpp
struct Handle {
    int fd_;
    ~Handle() { close(fd_); }
    // no move declared -> moves silently become COPIES
    // -> two Handles holding the same fd -> double close
};
```

### the three assertions that pin it down

```cpp
struct HasDtor { ~HasDtor() {} std::string s; };

static_assert(std::is_move_constructible_v<HasDtor>);
    // it LOOKS movable...
static_assert(!std::is_nothrow_move_constructible_v<HasDtor>);
    // ...but that's the COPY ctor answering the phone
static_assert(std::is_nothrow_move_constructible_v<decltype(HasDtor::s)>);
    // even though the only member moves just fine
```

Read those three together. The type reports itself as move-constructible.
It isn't, really.

---

## the special member functions

Six of them:

```cpp
struct T {
    T();                           // default constructor
    ~T();                          // destructor
    T(const T&);                   // copy constructor
    T& operator=(const T&);        // copy assignment
    T(T&&) noexcept;               // move constructor
    T& operator=(T&&) noexcept;    // move assignment
};
```

The compiler generates them for you, unless you interfere.

### rule of zero

**Declare none of them.** If every member manages its own resources
(`std::string`, `std::vector`, `std::unique_ptr`), the compiler-generated
versions are correct *and* optimal.

```cpp
struct Message {
    MessageId                 id;
    std::string               text;
    std::vector<ImageContent> images;
    // nothing declared. all six generated. all correct.
};
```

That's what agentty's domain types do, and it's the target. If you find
yourself writing a destructor for a domain type, ask whether a member
should be a smart pointer instead.

### rule of five

**If you declare one, declare all five.** You just measured why.

```cpp
struct Handle {
    int fd_ = -1;

    explicit Handle(int fd) noexcept : fd_(fd) {}
    ~Handle() { if (fd_ >= 0) close(fd_); }

    Handle(const Handle&)            = delete;   // can't copy an fd
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) close(fd_); fd_ = std::exchange(o.fd_, -1); }
        return *this;
    }
};
```

`std::exchange(o.fd_, -1)` returns the old value and sets the new one in
a single expression. It's the idiomatic way to write a move that has to
leave the source in a safe state. Chapter 2 covers this properly.

### `= default` and `= delete`

```cpp
struct MoveOnly {
    MoveOnly(const MoveOnly&)                = delete;
    MoveOnly& operator=(const MoveOnly&)     = delete;
    MoveOnly(MoveOnly&&) noexcept            = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};
```

`= default` gives you the compiler's version explicitly, and keeps it
*trivial* where possible, which matters for optimisation. `= delete`
makes using it a compile error with a clear message instead of a
confusing "no matching function" wall.

---

## push_back versus emplace_back

```cpp
static void push_vs_emplace() {
    std::puts("\n-- push_back vs emplace_back --");
    std::vector<Tracked> v;
    v.reserve(4);                    // no reallocation noise

    std::puts(" v.push_back(Tracked{\"P\"}):");
    v.push_back(Tracked{"P"});       // ctor, then move

    std::puts(" v.emplace_back(\"E\"):");
    v.emplace_back("E");             // ctor, in place, done

    std::puts(" scope end:");
}
```

```
-- push_back vs emplace_back --
 v.push_back(Tracked{"P"}):
  ctor      P
  move-ctor P
  dtor      (moved-from)
 v.emplace_back("E"):
  ctor      E
 scope end:
  dtor      P
  dtor      E
```

`push_back` takes a built object and moves it in: construct, move, destroy
the husk. `emplace_back` takes the *constructor arguments* and builds the
object directly in the vector's storage: one construct.

Note the `v.reserve(4)` first. Without it you'd also see reallocation
moves mixed in and the comparison would be muddy. Reserving when you know
the size is good practice anyway.

**Use `emplace_back` when constructing in place. Use `push_back` when you
already have the object.**

Two caveats:

1. `emplace_back` uses **direct** initialisation, so it can call
   `explicit` constructors. Usually what you want, but it means it won't
   catch a mistake that `push_back` would reject.
2. If you already have an lvalue, `emplace_back(x)` and `push_back(x)`
   both copy. `emplace` isn't magic.

---

## now break it

1. Delete `std::move` from `Tracked`'s move constructor (make it
   `name(o.name)`). Rerun. Watch your move constructor print `move-ctor`
   while actually copying.
2. Remove `noexcept` from `Tracked`'s move ctor, then push 10 elements
   without reserving. Count copies.
3. Write the `NoCopyNoMove` example above and confirm it compiles.
4. Add `~Tracked() = default;` to a *fresh* struct that otherwise
   declares nothing, and check
   `std::is_nothrow_move_constructible_v` on it. You might expect
   `= default` to be harmless. It isn't — **any user-declared destructor
   suppresses the implicit moves, even a defaulted one.** Verify that,
   then work out what it means for a class you thought was safe.
5. Replace `v.reserve(4)` with nothing and rerun `push_vs_emplace`.
   Explain every extra line.

---

## check yourself

1. How many constructor calls does `auto x = make_rvo();` cost?
2. Why does `return std::move(local);` make things slower?
3. Why does `vector` copy instead of moving when the move ctor isn't
   `noexcept`?
4. What does `emplace_back` save over `push_back`?
5. What's the rule of zero, and when does it not apply?
6. What happens to the implicit move operations if you declare a
   destructor?

<details>
<summary>answers</summary>

1. One. C++17 guarantees the prvalue is constructed directly in the
   caller's storage — no copy and no move, even conceptually.
2. It turns the named local into an xvalue, and copy elision requires the
   expression to name the local. So you get a real move instead of nothing
   at all.
3. Because a throwing move during reallocation leaves the already-moved
   elements gutted with no way to restore them, breaking the strong
   exception guarantee that `push_back` promises.
4. The temporary and the move: `emplace_back` constructs directly in the
   vector's storage from the raw arguments.
5. Declare none of the six special members and let the compiler generate
   them. It applies whenever every member manages its own resource. It
   doesn't apply when you hold a raw resource like a file descriptor.
6. They're suppressed. Moves silently fall back to copies, with no
   warning.

</details>

---

[next: auto and decltype →](09-auto-and-decltype.md)
