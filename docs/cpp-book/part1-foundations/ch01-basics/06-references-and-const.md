[← initialisation](05-initialisation.md) · [chapter index](README.md) · [next: lifetime →](07-lifetime.md)

# 6. References and const

**Time:** 75 minutes
**Code:** [`code/06_references.cpp`](code/06_references.cpp)
**Real file:** [`include/agentty/domain/lazy_bytes.hpp`](../../../../include/agentty/domain/lazy_bytes.hpp)

```sh
cd code && make 06_references && ./06_references
```

---

## a reference is an alias

```
-- a reference is an alias, not a pointer --
x = 99 after writing through r
&x == &r ? yes
you cannot rebind r. `r = y` writes y's value INTO x.
a reference has no null state and must be initialised.
```

```cpp
int  x = 1;
int& r = x;     // r IS x. not "points to". is.
r = 99;         // x is now 99
```

`&x == &r` because there is one object. A reference isn't a separate thing
holding an address — it's a second name for the same storage.

Three consequences:

1. **Must be initialised.** `int& r;` is a compile error. There is no
   "not referring to anything yet".
2. **Can't be rebound.** `r = y` doesn't make `r` refer to `y`; it copies
   `y`'s value into `x`. Once bound, always bound.
3. **Can't be null.** So a reference parameter documents "this is always
   here", and the caller can't hand you nothing.

If you need "maybe there, and reassignable", that's a pointer, or
`std::optional<std::reference_wrapper<T>>`, or just redesign.

(Under the hood the compiler usually implements a reference as an address,
same as a pointer. That's an implementation detail. The *semantics* are
alias semantics and that's what you reason with.)

---

## how to pass parameters

```
-- passing --
  ctor  payload
 by_value(n):
  COPY  payload
  in by_value:  payload
  dtor  payload
 by_cref(n):
  in by_cref:   payload
 by_ref(n) then read:
  n.name = payload!
 end of scope:
  dtor  payload!
```

Look at what printed. `by_value` printed `COPY` and then a `dtor` for the
copy. `by_cref` printed nothing extra — no copy happened at all.

The decision table:

| you want to | signature | cost |
|---|---|---|
| read it | `const T&` | nothing |
| modify caller's object | `T&` | nothing |
| keep a copy | `T` (then move into place) | one copy or one move |
| take ownership of an rvalue | `T&&` | nothing |
| read something small and trivial | `T` | a register |
| read a string you won't store | `std::string_view` | nothing |

### the "small and trivial" carve-out

For `int`, `double`, a pointer, a two-word struct — pass by value. A
`const int&` is *slower*: it forces the value into memory so there's an
address to take, and then every read is an indirection. The rule of thumb
is roughly "two pointers' worth or less, and trivially copyable, pass by
value".

### the sink parameter

When the function is going to keep the value, take it **by value** and
move it into place:

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

```cpp
ThreadId a{some_string};              // caller copies, ctor moves  -> 1 copy
ThreadId b{std::move(some_string)};   // caller moves,  ctor moves  -> 0 copies
```

One signature, optimal for both. The alternative — writing both a
`const T&` and a `T&&` overload — doubles for each parameter, so a
three-parameter constructor becomes eight overloads.

### `string_view` for read-only string parameters

```cpp
void log(std::string_view msg);        // takes string, const char*, literal
```

No allocation, no copy, accepts every string-shaped thing. Just never
*store* one — section 7 explains why.

---

## const extends a temporary's life

```
-- const& extends a temporary --
r = 'temporary'   still alive
```

```cpp
const std::string& r = make();   // make() returns by value
// the temporary lives as long as r does
```

Normally a temporary dies at the end of the full expression. Binding it
directly to a `const&` (or a `T&&`) extends its lifetime to match the
reference. This is why `const T&` parameters can take temporaries safely.

**The limit:** extension only applies to the temporary *directly bound*.
Not to a member of it:

```cpp
struct Wrap { std::string s; };
Wrap make_wrap();

const std::string& bad = make_wrap().s;   // DANGLES. the Wrap dies.
```

And it doesn't survive a return:

```cpp
const std::string& f() {
    const std::string& r = make();   // extended to r's lifetime
    return r;                        // which ends HERE. caller gets garbage.
}
```

---

## const is a property of the path, not the object

```
-- const is a property of the ACCESS PATH --
through v: 'mutable object'
through v: 'mutable object changed underneath'
v promised not to write. it never promised nobody else would.
```

```cpp
std::string s = "mutable object";
const std::string& v = s;      // a read-only VIEW of a writable object
s += " changed underneath";    // legal! s is not const.
// v now reads the new value.
```

`const` on a reference means *you* won't write through this path. It says
nothing about the object. Other paths can still modify it.

That matters for two reasons:

- **Thread safety:** `const` is not `immutable`. A `const&` to something
  another thread is writing is still a data race. `const` gives you no
  synchronisation.
- **Caching:** you can't cache a value you read through a `const&` and
  assume it stays valid, if something else holds a mutable path to it.

The immutable version is `const std::string s = "x";` — the *object* is
const, and modifying it through any path is UB.

---

## const member functions

```cpp
class Thread {
    std::size_t size() const;       // promises not to modify
    void        add(Message m);     // may modify
};
```

Inside a `const` member function, `this` is `const Thread*`, so every
member is const and you can't call non-const members.

The overload pair:

```cpp
const Message& at(std::size_t i) const { return msgs_[i]; }   // read
Message&       at(std::size_t i)       { return msgs_[i]; }   // write
```

The compiler picks based on the constness of the object. This is how
`std::vector::operator[]` works.

**Mark const aggressively.** A missing const propagates outward: callers
that had a `const&` now can't call you, so they drop const, so *their*
callers drop it. By the time you notice, nothing in the codebase is const
and adding it back is a week of work.

---

## `mutable`, and the one time it's honest

Now the interesting part.

```
-- mutable, and why it is honest here --
  (resolving blob 'img-7f3a' ... )
first  bytes(): decoded:img-7f3a
second bytes(): decoded:img-7f3a
second call did not resolve. same answer both times, so from
the caller's side nothing changed.
```

`mutable` on a member means "this can be written even through a const
path". It's usually a smell. Here it's the correct answer, and agentty's
`LazyBytes` is the reason.

### the problem it solves

agentty threads carry images. Pasted screenshots, tool results, attached
files. A long thread can hold megabytes of PNG.

Loading a thread from disk used to decode every image eagerly. Open a
thread with forty screenshots and you'd sit there while base64 decoded
forty payloads you probably weren't going to look at.

So `LazyBytes` holds either the bytes or *a way to get the bytes*:

```cpp
// include/agentty/domain/lazy_bytes.hpp
class LazyBytes {
public:
    struct Source {
        std::string blob;   // content-addressed blob name (preferred)
        std::string b64;    // legacy inline base64 fallback
        [[nodiscard]] bool empty() const noexcept {
            return blob.empty() && b64.empty();
        }
    };

    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            bytes_    = resolver_ ? resolver_(source_) : std::string{};
            resolved_ = true;
        }
        return bytes_;
    }

private:
    inline static Resolver resolver_ = nullptr;

    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
};
```

### why `bytes()` has to be const

Messages are read through const references everywhere. The renderer gets a
`const Message&`. The provider serialiser gets a `const Thread&`. Neither
of them is modifying anything, and both need the pixels.

If `bytes()` were non-const, every one of those call sites would need a
mutable path to the message — which means the render path could
accidentally mutate the conversation. That's a much worse problem than a
`mutable` member.

### why the `mutable` is legitimate

The test for "is this `mutable` honest" is **logical constness**: does the
observable behaviour of the object change?

Here, no:

- `bytes()` returns the same bytes on every call. First call and
  hundredth call are indistinguishable from outside.
- A copy copies the `source_` too, so the copy resolves to the same bytes.
  Copying isn't observable either.
- A failed resolve (deleted blob, corrupt base64) gives you empty bytes —
  exactly what the old eager loader produced for the same input, and every
  consumer already handles an empty payload.

The header spells those out as explicit invariants, which is what makes
the `mutable` reviewable rather than a vibe.

### the part that's easy to miss

```cpp
[[nodiscard]] bool empty() const noexcept {
    return resolved_ ? bytes_.empty() : source_.empty();
}
```

`empty()` does **not** call `bytes()`. If it did, every "is there an image
here" check would materialise the payload and the whole lazy scheme would
buy nothing.

Same for `source()` and `materialised()`. The writer uses them to
re-persist an untouched payload **by reference** — no decode, no
re-encode, the blob name just gets written back out. Saving a thread you
only scrolled through touches zero image bytes.

That's the real win, and it only works because the cheap questions stay
cheap.

### when `mutable` is a lie

- a "cache" that returns different values on different calls
- a hit counter that callers can observe
- anything where two const calls give different answers
- **anything touched from more than one thread without a mutex** — this is
  the big one. `mutable` makes a const object writable, which means a
  `const&` is no longer safe to share across threads. `LazyBytes` gets
  away with it because agentty resolves on one thread.

If you can't write down the invariant the way `lazy_bytes.hpp` does, you
probably want a non-const function.

---

## check yourself

1. Why can't you rebind a reference?
2. `const std::string& r = make();` — when does the temporary die?
3. Does `const std::string& v = s;` stop `s` from changing?
4. Why does `LazyBytes::bytes()` have to be `const`?
5. Why doesn't `LazyBytes::empty()` call `bytes()`?
6. When is `mutable` a bad idea?

<details>
<summary>answers</summary>

1. Because `r = y` is defined as assigning through the reference, writing
   `y`'s value into the referenced object. There's no syntax to change
   what it aliases.
2. When `r` goes out of scope. Direct binding to a `const&` extends the
   temporary's lifetime.
3. No. It only stops *you* from writing through `v`. `s` itself isn't
   const.
4. Because Messages are read through `const&` everywhere — the renderer
   and the serialiser both need bytes and neither should hold a mutable
   path to the conversation.
5. Because that would materialise the payload just to answer "is there
   anything here", defeating the entire point of being lazy.
6. When two const calls can observe different results, or when the object
   is shared across threads without synchronisation.

</details>

---

[next: lifetime →](07-lifetime.md)
