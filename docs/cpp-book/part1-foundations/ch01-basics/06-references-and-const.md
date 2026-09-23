[← initialisation](05-initialisation.md) · [chapter index](README.md) · [next: lifetime →](07-lifetime.md)

# 6. References and const

**Time:** 75 minutes. Type everything.
**Real file:** [`include/agentty/domain/lazy_bytes.hpp`](../../../../include/agentty/domain/lazy_bytes.hpp)

By the end of this section you'll have built agentty's lazy-payload
pattern yourself, and you'll know why a `mutable` member is the right
answer there when it's usually the wrong one.

Open `06_references.cpp`.

---

## a reference is an alias

```cpp
#include <cstdio>
#include <string>

static void aliasing() {
    std::puts("-- a reference is an alias, not a pointer --");
    int  x = 1;
    int& r = x;            // r IS x
    r = 99;
    std::printf("x = %d after writing through r\n", x);
    std::printf("&x == &r ? %s\n", (&x == &r) ? "yes" : "no");
    std::puts("you cannot rebind r. `r = y` writes y's value INTO x.");
    std::puts("a reference has no null state and must be initialised.");
}

int main() {
    aliasing();
}
```

```
-- a reference is an alias, not a pointer --
x = 99 after writing through r
&x == &r ? yes
```

`&x == &r` because **there is one object**. A reference isn't a separate
thing holding an address — it's a second name for the same storage.

Three consequences, and all three are worth testing yourself:

**1. Must be initialised.**

```cpp
int& r;        // error: 'r' declared as reference but not initialized
```

There's no "not referring to anything yet" state.

**2. Can't be rebound.**

```cpp
int x = 1, y = 2;
int& r = x;
r = y;             // does NOT make r refer to y.
                   // copies y's VALUE into x. x is now 2.
```

Try it. Print `x`, `y`, and `&r` afterward. `&r` is still `&x`. Once
bound, always bound.

**3. Can't be null.**

So a reference parameter documents "this is always here" and the caller
physically cannot hand you nothing. If you need "maybe there, and
reassignable", that's a pointer — or a redesign.

> Under the hood the compiler usually implements a reference as an
> address, same as a pointer. That's an implementation detail. The
> *semantics* are alias semantics, and that's what you reason with.

---

## how to pass parameters

Add a type that reports what happens to it:

```cpp
struct Noisy {
    std::string name;
    explicit Noisy(std::string n) : name(std::move(n)) {
        std::printf("  ctor  %s\n", name.c_str());
    }
    Noisy(const Noisy& o) : name(o.name) {
        std::printf("  COPY  %s\n", name.c_str());
    }
    Noisy(Noisy&& o) noexcept : name(std::move(o.name)) {
        std::printf("  move  %s\n", name.c_str());
    }
    ~Noisy() { std::printf("  dtor  %s\n", name.empty() ? "(moved-from)" : name.c_str()); }
};

static void by_value(Noisy n)        { std::printf("  in by_value:  %s\n", n.name.c_str()); }
static void by_cref(const Noisy& n)  { std::printf("  in by_cref:   %s\n", n.name.c_str()); }
static void by_ref(Noisy& n)         { n.name += "!"; }

static void parameter_passing() {
    std::puts("\n-- passing --");
    Noisy n{"payload"};

    std::puts(" by_value(n):");
    by_value(n);                    // copies in, destroys at end of call

    std::puts(" by_cref(n):");
    by_cref(n);                     // nothing printed = nothing copied

    std::puts(" by_ref(n) then read:");
    by_ref(n);
    std::printf("  n.name = %s\n", n.name.c_str());

    std::puts(" end of scope:");
}
```

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

`by_value` printed `COPY` and then a `dtor` for the copy. `by_cref`
printed nothing extra — **no copy happened at all**.

### the decision table

| you want to | signature | cost |
|---|---|---|
| read it | `const T&` | nothing |
| modify caller's object | `T&` | nothing |
| keep a copy | `T` (then move into place) | one copy or one move |
| take ownership of an rvalue | `T&&` | nothing |
| read something small and trivial | `T` | a register |
| read a string you won't store | `std::string_view` | nothing |

### the "small and trivial" carve-out

For `int`, `double`, a pointer, a two-word struct — **pass by value**. A
`const int&` is actually *slower*: it forces the value into memory so
there's an address to take, and then every read is an indirection.

Rule of thumb: two pointers' worth or less, and trivially copyable → by
value.

### the sink parameter

When the function is going to *keep* the value, take it by value and move
it into place:

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

```cpp
ThreadId a{some_string};              // caller copies, ctor moves  -> 1 copy
ThreadId b{std::move(some_string)};   // caller moves,  ctor moves  -> 0 copies
```

One signature, optimal for both. The alternative — a `const T&` overload
*and* a `T&&` overload — doubles for every parameter, so a
three-parameter constructor needs eight overloads.

### `string_view` for read-only string parameters

```cpp
void log(std::string_view msg);      // takes string, const char*, literal
```

No allocation, no copy, accepts every string-shaped thing. Just never
*store* one — section 7 explains why.

---

## const extends a temporary's life

```cpp
static std::string make() { return "temporary"; }

static void const_ref_lifetime() {
    std::puts("\n-- const& extends a temporary --");
    const std::string& r = make();   // the temporary lives as long as r
    std::printf("r = '%s'   still alive\n", r.c_str());
}
```

```
-- const& extends a temporary --
r = 'temporary'   still alive
```

Normally a temporary dies at the end of the full expression. Binding it
**directly** to a `const&` (or a `T&&`) extends its lifetime to match the
reference. That's why `const T&` parameters can accept temporaries safely.

### the two limits

**Extension applies only to the temporary directly bound.** Not to a
member of it:

```cpp
struct Wrap { std::string s; };
Wrap make_wrap();

const std::string& bad = make_wrap().s;   // DANGLES. the Wrap still dies.
```

**And it doesn't survive a return:**

```cpp
const std::string& f() {
    const std::string& r = make();   // extended to r's lifetime
    return r;                        // which ends HERE. caller gets garbage.
}
```

Both of those are worth writing and running under ASan. The report is the
lesson.

---

## const is a property of the path, not the object

```cpp
static void const_views() {
    std::puts("\n-- const is a property of the ACCESS PATH --");
    std::string s = "mutable object";
    const std::string& v = s;        // read-only view of a mutable object
    std::printf("through v: '%s'\n", v.c_str());
    s += " changed underneath";      // legal: s itself is not const
    std::printf("through v: '%s'\n", v.c_str());
    std::puts("v promised not to write. it never promised nobody else would.");
}
```

```
-- const is a property of the ACCESS PATH --
through v: 'mutable object'
through v: 'mutable object changed underneath'
```

`const` on a reference means **you** won't write through *this path*. It
says nothing about the object. Other paths can still modify it.

That matters for two real reasons:

- **Thread safety.** `const` is not `immutable`. A `const&` to something
  another thread is writing is still a data race. `const` gives you
  exactly zero synchronisation.
- **Caching.** You can't read a value through a `const&` and assume it
  stays valid, if something else holds a mutable path.

The genuinely immutable version is `const std::string s = "x";` — the
*object* is const, and modifying it through any path is UB.

### const member functions

```cpp
class Thread {
    std::size_t size() const;       // promises not to modify
    void        add(Message m);     // may modify
};
```

Inside a `const` member function, `this` is `const Thread*`, so every
member is const and you can't call non-const members.

The overload pair, which is how `std::vector::operator[]` works:

```cpp
const Message& at(std::size_t i) const { return msgs_[i]; }   // read
Message&       at(std::size_t i)       { return msgs_[i]; }   // write
```

The compiler picks based on the constness of the object.

**Mark const aggressively, from the start.** A missing `const` propagates
outward: callers that had a `const&` now can't call you, so they drop
const, so *their* callers drop it. By the time you notice, nothing in the
codebase is const.

---

## `mutable`, and the one time it's honest

Now the interesting part. This is agentty's `LazyBytes` in miniature —
type it, then we'll look at the real one.

```cpp
class Payload {
public:
    explicit Payload(std::string blob_name) : blob_(std::move(blob_name)) {}

    // const, because materialising is not a LOGICAL mutation. the caller
    // asked for bytes; where they came from is our business.
    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            std::printf("  (resolving blob '%s' ... )\n", blob_.c_str());
            bytes_    = "decoded:" + blob_;
            resolved_ = true;
        }
        return bytes_;
    }

private:
    std::string         blob_;
    mutable std::string bytes_;          // mutable = writable through const
    mutable bool        resolved_ = false;
};

static void mutable_cache() {
    std::puts("\n-- mutable, and why it is honest here --");
    const Payload p{"img-7f3a"};         // note: const
    std::printf("first  bytes(): %s\n", p.bytes().c_str());
    std::printf("second bytes(): %s\n", p.bytes().c_str());
}
```

```
-- mutable, and why it is honest here --
  (resolving blob 'img-7f3a' ... )
first  bytes(): decoded:img-7f3a
second bytes(): decoded:img-7f3a
```

Note two things. `p` is **`const`**, and `bytes()` still works. And the
resolve message printed **once** — the second call hit the cache.

Try removing `mutable` from `bytes_` and `resolved_`:

```
error: no match for 'operator=' (operand types are 'const std::string'
and 'std::__cxx11::basic_string<char>')
```

Not the clearest message gcc has ever produced. What it means is: inside a
`const` member function every member is `const`, so `bytes_` is a
`const std::string`, and a `const std::string` has no assignment operator
that accepts anything. Read past the template noise and it's just "you
tried to write to a read-only member".

`mutable` on a member means "this can be written even through a const
path". It's usually a smell. Here it's the correct answer.

---

## the real thing

agentty threads carry images. Pasted screenshots, tool results, attached
files. A long thread can hold megabytes of PNG.

Loading a thread used to decode every image eagerly. Open a thread with
forty screenshots and you'd sit there while base64 decoded forty payloads
you probably weren't going to look at. Then saving it re-encoded all
forty to write them back.

Both of those are pure waste. Here's the fix, from
`include/agentty/domain/lazy_bytes.hpp`:

```cpp
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

    [[nodiscard]] bool empty() const noexcept {
        return resolved_ ? bytes_.empty() : source_.empty();
    }
    [[nodiscard]] const Source& source() const noexcept { return source_; }
    [[nodiscard]] bool materialised() const noexcept { return resolved_; }

private:
    inline static Resolver resolver_ = nullptr;

    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
};
```

### why `bytes()` has to be const

Messages are read through const references **everywhere**. The renderer
gets a `const Message&`. The provider serialiser gets a `const Thread&`.
Neither is modifying anything, and both need the pixels.

If `bytes()` were non-const, every one of those call sites would need a
mutable path to the message — which means the render path could
accidentally mutate the conversation. That is a far worse problem than a
`mutable` member.

### the test for an honest `mutable`

**Logical constness**: does the observable behaviour of the object change?

Here, no, and the header states all three reasons as explicit invariants:

- **idempotent** — `bytes()` returns the same bytes on every call. First
  call and hundredth call are indistinguishable from outside.
- **copy-safe** — a copy copies `source_` too, so the copy resolves to the
  same bytes rather than to nothing.
- **failure-equivalent** — a failed resolve (deleted blob, corrupt base64)
  gives empty bytes, which is *exactly* what the old eager loader produced
  for the same input. Every consumer already handles an empty payload.

Being able to write those three lines down is what makes the `mutable`
reviewable instead of a vibe.

### the part that's easy to miss

```cpp
[[nodiscard]] bool empty() const noexcept {
    return resolved_ ? bytes_.empty() : source_.empty();
}
```

**`empty()` does not call `bytes()`.**

If it did, every "is there an image here" check would materialise the
payload and the whole lazy scheme would buy nothing.

The branch answers from whichever side is authoritative: if we've
resolved, ask the bytes; if not, ask the source.

Same for `source()` and `materialised()`. The persistence writer uses
them:

```cpp
if (!img.materialised())
    write_blob_reference(img.source().blob);   // no decode, no re-encode
else
    write_blob(img.bytes());
```

Saving a thread you only scrolled through touches **zero** image bytes.
That's the real payoff, and it exists only because the cheap questions
stayed cheap.

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

## the whole file

```cpp
// 06_references.cpp — references, const, and how to pass parameters.

#include <cstdio>
#include <string>

struct Noisy {
    std::string name;
    explicit Noisy(std::string n) : name(std::move(n)) {
        std::printf("  ctor  %s\n", name.c_str());
    }
    Noisy(const Noisy& o) : name(o.name) {
        std::printf("  COPY  %s\n", name.c_str());
    }
    Noisy(Noisy&& o) noexcept : name(std::move(o.name)) {
        std::printf("  move  %s\n", name.c_str());
    }
    ~Noisy() { std::printf("  dtor  %s\n", name.empty() ? "(moved-from)" : name.c_str()); }
};

// ── a reference is another name for the same object ────────────────────
static void aliasing() {
    std::puts("-- a reference is an alias, not a pointer --");
    int  x = 1;
    int& r = x;            // r IS x
    r = 99;
    std::printf("x = %d after writing through r\n", x);
    std::printf("&x == &r ? %s\n", (&x == &r) ? "yes" : "no");
    std::puts("you cannot rebind r. `r = y` writes y's value INTO x.");
    std::puts("a reference has no null state and must be initialised.");
}

// ── const reference: read-only view, no copy ───────────────────────────
static void by_value(Noisy n)        { std::printf("  in by_value:  %s\n", n.name.c_str()); }
static void by_cref(const Noisy& n)  { std::printf("  in by_cref:   %s\n", n.name.c_str()); }
static void by_ref(Noisy& n)         { n.name += "!"; }

static void parameter_passing() {
    std::puts("\n-- passing --");
    Noisy n{"payload"};

    std::puts(" by_value(n):");
    by_value(n);

    std::puts(" by_cref(n):");
    by_cref(n);

    std::puts(" by_ref(n) then read:");
    by_ref(n);
    std::printf("  n.name = %s\n", n.name.c_str());

    std::puts(" end of scope:");
}

// ── const binds to temporaries and extends their life ──────────────────
static std::string make() { return "temporary"; }

static void const_ref_lifetime() {
    std::puts("\n-- const& extends a temporary --");
    const std::string& r = make();
    std::printf("r = '%s'   still alive\n", r.c_str());
    std::puts("this ONLY works when the const& binds the temporary directly.");
    std::puts("a reference to a MEMBER of a temporary is not extended.");
}

// ── const is about the view, not the object ────────────────────────────
static void const_views() {
    std::puts("\n-- const is a property of the ACCESS PATH --");
    std::string s = "mutable object";
    const std::string& v = s;
    std::printf("through v: '%s'\n", v.c_str());
    s += " changed underneath";
    std::printf("through v: '%s'\n", v.c_str());
    std::puts("v promised not to write. it never promised nobody else would.");
}

// ── mutable: the LazyBytes pattern ─────────────────────────────────────
class Payload {
public:
    explicit Payload(std::string blob_name) : blob_(std::move(blob_name)) {}

    [[nodiscard]] const std::string& bytes() const {
        if (!resolved_) {
            std::printf("  (resolving blob '%s' ... )\n", blob_.c_str());
            bytes_    = "decoded:" + blob_;
            resolved_ = true;
        }
        return bytes_;
    }

private:
    std::string         blob_;
    mutable std::string bytes_;
    mutable bool        resolved_ = false;
};

static void mutable_cache() {
    std::puts("\n-- mutable, and why it is honest here --");
    const Payload p{"img-7f3a"};
    std::printf("first  bytes(): %s\n", p.bytes().c_str());
    std::printf("second bytes(): %s\n", p.bytes().c_str());
    std::puts("second call did not resolve. same answer both times, so from");
    std::puts("the caller's side nothing changed. that is what makes the");
    std::puts("mutable legitimate: bitwise change, logical constness.");
}

int main() {
    aliasing();
    parameter_passing();
    const_ref_lifetime();
    const_views();
    mutable_cache();
}
```

---

## now break it

1. Remove `mutable` from `Payload::bytes_`. Read the error.
2. Remove `const` from `Payload::bytes()` and try to call it on the
   `const Payload p`. Read that error too — it's the one that would have
   forced a mutable path into the renderer.
3. Add an `empty()` to `Payload` that calls `bytes()`. Then call
   `p.empty()` and watch it resolve. That's the bug agentty avoids.
4. Change `by_cref` to take `Noisy` by value. Count the new `COPY` lines.
5. Write `const std::string& bad = make_wrap().s;` and run it under
   ASan.
6. Try `int& r; r = 5;` and read the error.

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

1. Because `r = y` is defined as assigning *through* the reference,
   writing `y`'s value into the referenced object. There's no syntax to
   change what it aliases.
2. When `r` goes out of scope. Direct binding to a `const&` extends the
   temporary's lifetime to match the reference.
3. No. It only stops *you* from writing through `v`. `s` itself isn't
   const.
4. Because Messages are read through `const&` everywhere — the renderer
   and the serialiser both need bytes, and neither should hold a mutable
   path to the conversation.
5. Because that would materialise the payload just to answer "is there
   anything here", defeating the entire point of being lazy.
6. When two const calls can observe different results, or when the object
   is shared across threads without synchronisation.

</details>

---

[next: lifetime →](07-lifetime.md)
