[← lifetime](07-lifetime.md) · [chapter index](README.md) · [next: auto and decltype →](09-auto-and-decltype.md)

# 8. Copy, move, elision

**Time:** 75 minutes
**Code:** [`code/08_copy_move.cpp`](code/08_copy_move.cpp)

```sh
cd code && make 08_copy_move && ./08_copy_move
```

The `Tracked` type in the example prints on every constructor, assignment,
and destructor. Every claim below is visible in the output.

---

## copy versus move

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

**Copy** duplicates. Both objects are independent and both own their
resources. Costs whatever the resource costs — for a `std::string` with a
megabyte in it, a megabyte of allocation and memcpy.

**Move** transfers. The source gives up ownership, the destination takes
it. For a `std::string` that's three pointer writes, regardless of size.

The output shows `a` ending up moved-from: its name is empty, because the
move constructor took its buffer.

---

## elision: the copy that never happens

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

Three functions that all return a `Tracked` by value. Watch how many
operations each costs.

### RVO: guaranteed since C++17

```cpp
Tracked make_rvo() {
    return Tracked{"RVO"};
}
auto r = make_rvo();
```

One `ctor`. That's it. No copy, no move.

In C++17 this isn't an optimisation, it's a language rule. A prvalue
returned from a function is not a temporary that gets copied — the object
is constructed *directly into the caller's storage*. There is only ever
one object. It works even for types with deleted copy and move
constructors.

**So return by value. Always.** Returning by value is free.

### NRVO: allowed, near-universal

```cpp
Tracked make_nrvo() {
    Tracked local{"NRVO"};
    return local;
}
```

Also one `ctor` in practice. "Named RVO" applies when you return a named
local, and the compiler constructs `local` directly in the caller's slot.

It's *permitted*, not guaranteed — the standard says the compiler may
elide, and gcc, clang, and msvc all do at `-O1` and above. Even if it
didn't, the fallback is a move (a named local in a return statement is
treated as an rvalue), which is cheap.

### the pessimisation

```cpp
Tracked make_pessimised() {
    Tracked local{"pessimised"};
    return std::move(local);      // NEVER
}
```

`ctor` + `move-ctor` + an extra `dtor`. You made it worse.

`return std::move(x)` turns the expression from an lvalue naming `local`
into an xvalue, and **elision requires a name**. The compiler can no
longer construct in place, so it must actually move.

gcc's `-Wpessimizing-move` catches this. The example suppresses it with a
pragma precisely so you can see the extra move in the output — the
warning is the lesson.

**Rule: never `std::move` a return value.** The one exception is returning
a *member* of a local object, where elision never applied anyway:

```cpp
std::string take() { return std::move(member_); }   // this one is fine
```

---

## `noexcept` on a move constructor is not optional

```
-- why noexcept on a move ctor is not optional --
 SafeMove, growing from 1 to 2:
  move (safe)
 RiskyMove, growing from 1 to 2:
  COPY (risky)
 is_nothrow_move_constructible<SafeMove>  = true
 is_nothrow_move_constructible<RiskyMove> = false
```

Two identical types. One's move constructor is `noexcept`, one's isn't.
When `std::vector` reallocates, the first gets *moved* and the second gets
*copied*.

### why vector does this

`vector::push_back` offers the **strong exception guarantee**: if it
throws, the vector is unchanged.

Reallocation means: allocate a new buffer, transfer N elements, free the
old one. Suppose element 5 of 10 throws during the transfer:

- If we were **copying**, the originals are all intact. Free the new
  buffer, rethrow, vector unchanged. Guarantee kept.
- If we were **moving**, elements 0–4 have been gutted and element 5 is
  half-built. We can't move them back — the moves back could throw too.
  The vector is destroyed. Guarantee broken.

So `vector` uses `std::move_if_noexcept`: move only when the move
constructor promises not to throw. Otherwise copy, and stay safe.

### what this costs

A `vector<std::string>` with 10,000 strings, growing. With `noexcept`
moves: 10,000 pointer swaps. Without: 10,000 allocations and memcpys. It's
an order of magnitude, and it shows up as "why is my program slow" with no
obvious cause.

### the rule

**Every move constructor and move assignment operator gets `noexcept`.**
If yours can't be, redesign until it can — usually that means holding a
pointer or a handle rather than something that allocates on move.

Every move in agentty is `noexcept`, including `Id`'s constructor:

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

You can check your own types:

```cpp
static_assert(std::is_nothrow_move_constructible_v<MyType>);
static_assert(std::is_nothrow_move_assignable_v<MyType>);
```

Put those in a test. They catch the regression the day it's introduced.

---

## push_back versus emplace_back

```
-- push_back vs emplace_back --
 v.push_back(Tracked{"P"}):
  ctor      P
  move-ctor P
  dtor      (moved-from)
 v.emplace_back("E"):
  ctor      E
```

`push_back` takes a built object and moves it in: construct, move,
destroy the husk. `emplace_back` takes the *constructor arguments* and
builds the object directly in the vector's storage: one construct.

```cpp
v.push_back(Tracked{"P"});    // ctor + move + dtor
v.emplace_back("E");          // ctor
```

**Use `emplace_back` when you're constructing in place. Use `push_back`
when you already have the object.**

Two caveats:

1. `emplace_back` uses *direct* initialisation, so it can call `explicit`
   constructors. That's usually what you want, but it means it won't catch
   a mistake that `push_back` would.
2. If you already have an lvalue, `emplace_back(x)` and `push_back(x)`
   both copy. `emplace` isn't magic.

And `reserve()` first when you know the size. It skips the reallocation
entirely.

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
versions are correct and optimal.

```cpp
struct Message {
    MessageId                 id;
    std::string               text;
    std::vector<ImageContent> images;
    // nothing declared. all six generated. all correct.
};
```

This is what agentty's domain types do, and it's the target. If you find
yourself writing a destructor for a domain type, ask whether a member
should be a smart pointer instead.

### rule of five

**If you declare one, declare all five.** Declaring a destructor suppresses
the implicit move operations, so a class with a custom destructor and no
declared move operations silently *copies* where you expected moves.

```cpp
struct Handle {
    ~Handle() { close(fd_); }
    // no move ctor declared -> moves silently become copies
    // -> double close. crash.
};
```

Chapter 2 covers this properly. For now: if you write one, write all five,
or use `= default` / `= delete` to say what you mean.

### `= default` and `= delete`

```cpp
struct MoveOnly {
    MoveOnly(const MoveOnly&)            = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept        = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};
```

`= default` gives you the compiler's version explicitly (and keeps it
trivial, which matters for optimisation). `= delete` makes using it a
compile error with a clear message.

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
   caller's storage.
2. It turns the named local into an xvalue, and copy elision requires the
   name. So you get a real move instead of no operation at all.
3. Because a throwing move during reallocation leaves the already-moved
   elements gutted with no way to restore them, breaking the strong
   exception guarantee.
4. The temporary and the move: `emplace_back` constructs directly in the
   vector's storage.
5. Declare none of the six special members and let the compiler generate
   them. It applies whenever every member manages its own resource. It
   doesn't apply when you hold a raw resource like a file descriptor.
6. They're suppressed. Moves silently become copies.

</details>

---

[next: auto and decltype →](09-auto-and-decltype.md)
