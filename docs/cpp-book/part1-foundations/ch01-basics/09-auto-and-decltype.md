[← copy, move, elision](08-copy-move-elision.md) · [chapter index](README.md) · [next: capstone →](10-capstone-imagecontent.md)

# 9. auto and decltype

**Time:** 45 minutes. Type everything.

Open `09_auto.cpp`.

---

## auto drops references and top-level const

```cpp
#include <cstdio>
#include <type_traits>

static void auto_strips() {
    std::puts("-- auto strips ref and top-level const --");
    int              x = 1;
    int&             r = x;
    const int        c = 2;
    const int&       cr = c;

    auto a1 = r;    // int    (not int&)
    auto a2 = c;    // int    (not const int)
    auto a3 = cr;   // int
    a1 = a2 = a3 = 9;   // all writable, none affect x or c

    std::printf("after writing to the autos: x = %d, c = %d\n", x, c);
    std::printf("is_same<decltype(a1), int> = %s\n",
                std::is_same_v<decltype(a1), int> ? "true" : "false");

    auto& keeps_ref = r;            // int&
    keeps_ref = 77;
    std::printf("through auto& : x = %d\n", x);

    const auto& view = x;           // const int&
    std::printf("const auto& view = %d\n", view);
}

int main() {
    auto_strips();
}
```

```
-- auto strips ref and top-level const --
after writing to the autos: x = 1, c = 2
is_same<decltype(a1), int> = true
through auto& : x = 77
const auto& view = 77
```

`auto` deduces like a **by-value template parameter**: references are
stripped, and top-level `const` is stripped. You get a fresh, writable
copy.

The output proves it — writing 9 to all three autos left `x` and `c`
untouched.

Note `std::is_same_v<decltype(a1), int>` — that's how you ask the compiler
what a type actually is, instead of guessing. Keep it in your toolbox.

To keep the reference or the const, say so:

```cpp
auto&       ref  = r;     // int&
const auto& view = x;     // const int&
auto&&      fwd  = f();   // binds to anything (forwarding reference)
```

### "top-level" is doing work in that sentence

```cpp
const int* p = &x;
auto q = p;             // const int* -- the const is on the POINTEE, kept

int* const cp = &x;
auto r2 = cp;           // int* -- the const is on the POINTER, dropped
```

`auto` drops the const **on the thing being copied**. `const int*` reads
as "pointer to const int" — the pointer itself isn't const, so nothing is
dropped. `int* const` is "a const pointer", and copying it gives you a
writable pointer.

Verify both with `static_assert` rather than trusting me:

```cpp
static_assert(std::is_same_v<decltype(q),  const int*>);
static_assert(std::is_same_v<decltype(r2), int*>);
```

---

## the range-for copy

```cpp
#include <string>
#include <vector>

struct Chatty {
    std::string s;
    explicit Chatty(const char* c) : s(c) {}
    Chatty(const Chatty& o) : s(o.s) { std::printf("  COPY %s\n", s.c_str()); }
    Chatty(Chatty&&) noexcept = default;
};

static void range_for() {
    std::puts("\n-- range-for --");
    std::vector<Chatty> v;
    v.reserve(2);
    v.emplace_back("one");
    v.emplace_back("two");

    std::puts(" for (auto e : v)        <- copies each element:");
    for (auto e : v) (void)e;

    std::puts(" for (const auto& e : v) <- no copies:");
    for (const auto& e : v) (void)e;
    std::puts(" (nothing printed above means nothing was copied)");

    std::puts(" for (auto& e : v)       <- when you want to modify");
    for (auto& e : v) e.s += "*";
    std::printf(" now: %s %s\n", v[0].s.c_str(), v[1].s.c_str());
}
```

```
-- range-for --
 for (auto e : v)        <- copies each element:
  COPY one
  COPY two
 for (const auto& e : v) <- no copies:
 (nothing printed above means nothing was copied)
 for (auto& e : v)       <- when you want to modify
 now: one* two*
```

`for (auto e : v)` copies every element. For a `vector<std::string>` with
a thousand entries, that's a thousand allocations you didn't mean to make.

The three forms:

```cpp
for (const auto& e : v)    // read only. the default.
for (auto& e : v)          // modify in place.
for (auto e : v)           // I want my own copy. say it deliberately.
```

**Default to `const auto&`.** Write the other two when you have a reason.

For cheap types (`int`, a pointer, a small trivially-copyable struct), the
copy is free and `for (auto x : v)` is fine and reads better.

---

## the map trap

This one is nasty because you wrote a `const&` and *still* got a copy.

```cpp
#include <map>

static void map_range_for() {
    std::puts("\n-- the map trap --");
    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    for (const auto& [k, val] : m)
        std::printf("   %s -> %d\n", k.c_str(), val);
}
```

The version that costs you:

```cpp
for (const std::pair<std::string, int>& kv : m)    // COPIES EVERY ELEMENT
```

Here's why. `std::map<K,V>::value_type` is **`std::pair<const K, V>`** —
the key is const, because changing a key in place would break the tree
ordering.

You wrote `pair<string, int>`, which is a **different type**. A `const&`
can't bind to a different type, so the compiler creates a temporary of
your type, converts, binds the reference to *that*, and destroys it each
iteration. Silent copy of every key and value.

Prove the type identity yourself:

```cpp
static_assert(std::is_same_v<std::map<std::string, int>::value_type,
                             std::pair<const std::string, int>>);
static_assert(!std::is_same_v<std::map<std::string, int>::value_type,
                              std::pair<std::string, int>>);
```

The fix:

```cpp
for (const auto& [k, v] : m)     // correct, and shorter
```

`auto` deduces the real `value_type`, so no conversion happens.
Structured bindings on top make it readable.

**This is the strongest practical argument for `auto` in range-for:** the
type is easy to get subtly wrong, and being wrong costs a copy silently.

---

## decltype

```cpp
static int  val() { return 1; }
static int& ref() { static int n = 5; return n; }

static void decltypes() {
    std::puts("\n-- decltype --");
    int x = 0;
    std::printf("decltype(x)    is int   : %s\n",
                std::is_same_v<decltype(x), int> ? "yes" : "no");
    std::printf("decltype((x))  is int&  : %s   <- extra parens matter\n",
                std::is_same_v<decltype((x)), int&> ? "yes" : "no");
    std::printf("decltype(val()) is int  : %s\n",
                std::is_same_v<decltype(val()), int> ? "yes" : "no");
    std::printf("decltype(ref()) is int& : %s\n",
                std::is_same_v<decltype(ref()), int&> ? "yes" : "no");

    auto           a = ref();   // int  — the reference is dropped
    decltype(auto) d = ref();   // int& — the reference is kept
    d = 42;
    std::printf("after d = 42: ref() = %d, a = %d\n", ref(), a);
}
```

```
-- decltype --
decltype(x)    is int   : yes
decltype((x))  is int&  : yes   <- extra parens matter
decltype(val()) is int  : yes
decltype(ref()) is int& : yes
after d = 42: ref() = 42, a = 5
```

`decltype` gives you the **exact** declared type, keeping references and
const. Unlike `auto`, it strips nothing.

### the parens rule

```cpp
decltype(x)    // int   -- x is a NAME, gives the declared type
decltype((x))  // int&  -- (x) is an EXPRESSION, and it's an lvalue
```

For an expression (not a plain name), `decltype` gives:

- `T&` if it's an lvalue
- `T&&` if it's an xvalue
- `T` if it's a prvalue

Wrapping a name in parens makes it an expression. This is the classic
gotcha:

```cpp
decltype(auto) f() {
    int x = 0;
    return (x);     // deduces int&. returns a reference to a dead local.
}
```

That's a section-7 dangling bug caused by two characters.

### `decltype(auto)`

Look at the last two lines of the output. `a` is still 5; `d = 42`
changed the actual static inside `ref()`.

```cpp
auto           a = ref();   // int   -- reference dropped, a is a copy
decltype(auto) d = ref();   // int&  -- reference kept, d aliases n
```

`decltype(auto)` means "deduce, but use `decltype` rules". Its main use is
perfect forwarding of a return type:

```cpp
template <typename F, typename... Args>
decltype(auto) invoke(F&& f, Args&&... args) {
    return std::forward<F>(f)(std::forward<Args>(args)...);
}
```

If `f` returns a reference, so does `invoke`. With plain `auto` you'd
silently copy. Chapter 4 goes deeper.

---

## the whole file

```cpp
// 09_auto.cpp — auto, decltype, and the range-for copy you didn't ask for.

#include <cstdio>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

struct Chatty {
    std::string s;
    explicit Chatty(const char* c) : s(c) {}
    Chatty(const Chatty& o) : s(o.s) { std::printf("  COPY %s\n", s.c_str()); }
    Chatty(Chatty&&) noexcept = default;
};

// ── auto drops references and top-level const ──────────────────────────
static void auto_strips() {
    std::puts("-- auto strips ref and top-level const --");
    int              x = 1;
    int&             r = x;
    const int        c = 2;
    const int&       cr = c;

    auto a1 = r;    // int    (not int&)
    auto a2 = c;    // int    (not const int)
    auto a3 = cr;   // int
    a1 = a2 = a3 = 9;   // all writable, none affect x or c

    std::printf("after writing to the autos: x = %d, c = %d\n", x, c);
    std::printf("is_same<decltype(a1), int> = %s\n",
                std::is_same_v<decltype(a1), int> ? "true" : "false");

    auto& keeps_ref = r;            // int&
    keeps_ref = 77;
    std::printf("through auto& : x = %d\n", x);

    const auto& view = x;           // const int&
    std::printf("const auto& view = %d\n", view);
}

// ── the range-for trap ─────────────────────────────────────────────────
static void range_for() {
    std::puts("\n-- range-for --");
    std::vector<Chatty> v;
    v.reserve(2);
    v.emplace_back("one");
    v.emplace_back("two");

    std::puts(" for (auto e : v)        <- copies each element:");
    for (auto e : v) (void)e;

    std::puts(" for (const auto& e : v) <- no copies:");
    for (const auto& e : v) (void)e;
    std::puts(" (nothing printed above means nothing was copied)");

    std::puts(" for (auto& e : v)       <- when you want to modify");
    for (auto& e : v) e.s += "*";
    std::printf(" now: %s %s\n", v[0].s.c_str(), v[1].s.c_str());
}

// ── the map one, which costs a copy for a subtler reason ───────────────
static void map_range_for() {
    std::puts("\n-- the map trap --");
    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    std::puts(" value_type is pair<const string, int>, NOT pair<string,int>.");
    std::puts(" so `for (const std::pair<std::string,int>& kv : m)` copies");
    std::puts(" every element to convert the type. use auto&:");
    for (const auto& [k, val] : m)
        std::printf("   %s -> %d\n", k.c_str(), val);
}

// ── decltype vs decltype(auto) ─────────────────────────────────────────
static int  val() { return 1; }
static int& ref() { static int n = 5; return n; }

static void decltypes() {
    std::puts("\n-- decltype --");
    int x = 0;
    std::printf("decltype(x)    is int   : %s\n",
                std::is_same_v<decltype(x), int> ? "yes" : "no");
    std::printf("decltype((x))  is int&  : %s   <- extra parens matter\n",
                std::is_same_v<decltype((x)), int&> ? "yes" : "no");
    std::printf("decltype(val()) is int  : %s\n",
                std::is_same_v<decltype(val()), int> ? "yes" : "no");
    std::printf("decltype(ref()) is int& : %s\n",
                std::is_same_v<decltype(ref()), int&> ? "yes" : "no");

    auto           a = ref();   // int  — the reference is dropped
    decltype(auto) d = ref();   // int& — the reference is kept
    d = 42;
    std::printf("after d = 42: ref() = %d, a = %d\n", ref(), a);
}

// ── when auto helps and when it hides ──────────────────────────────────
static void style() {
    std::puts("\n-- style --");
    std::vector<std::string> v{"x"};
    for (auto it = v.begin(); it != v.end(); ++it)  // good: name is noise
        std::printf("iterator auto: %s\n", it->c_str());

    auto n = v.size();                              // fine: size_t is obvious
    std::printf("size = %zu\n", n);

    std::puts("use auto when the type is long and obvious from the right side.");
    std::puts("write the type when it is the interesting part of the line.");
}

int main() {
    auto_strips();
    range_for();
    map_range_for();
    decltypes();
    style();
}
```

---

## when to use auto

**Yes:**

```cpp
auto it = container.begin();              // the type is noise
auto p = std::make_unique<Widget>(args);  // it's on the right already
auto lam = [](int x) { return x * 2; };   // lambdas have no spellable type
for (const auto& [k, v] : map) { }        // avoids the map trap
```

**No:**

```cpp
auto x = compute();       // what IS it? the reader has to go look.
auto n = v.size() - 1;    // hides that this is unsigned. see section 2.
auto f = 1 / 3;           // int. 0. the type was the interesting part.
```

**The test:** would a reader of this line have to go look up the type to
understand what the line does? If yes, write it out.

---

## now break it

1. Change `for (const auto& e : v)` to
   `for (const Chatty& e : v)`. Does it still avoid the copy? (It should
   — same type.) Now try `for (const std::pair<std::string,int>& kv : m)`
   on the map and count copies.
2. Add `std::printf` to `Chatty`'s move constructor and rerun. Which
   loops move?
3. Write `decltype(auto) f() { int x = 0; return (x); }` and run it under
   ASan.
4. Write the four `static_assert`s from the pointer-const section and
   flip one to see the failure message.
5. Change `decltype(auto) d = ref();` to `auto d = ref();` and rerun.
   Explain why `ref()` no longer changes.

---

## check yourself

1. `const int& cr = c; auto a = cr;` — what type is `a`?
2. How do you keep the reference and const?
3. Why does `for (const std::pair<std::string,int>& kv : m)` copy?
4. What's the difference between `decltype(x)` and `decltype((x))`?
5. When would you use `decltype(auto)`?
6. Is `auto n = v.size() - 1;` a good idea?

<details>
<summary>answers</summary>

1. `int`. `auto` strips both the reference and the top-level const.
2. `const auto& a = cr;`
3. `map::value_type` is `pair<const K, V>`, a different type, so each
   element gets converted into a temporary that the reference binds to.
4. `decltype(x)` on a name gives the declared type (`int`).
   `decltype((x))` on a parenthesised expression gives `int&` because the
   expression is an lvalue.
5. When you're forwarding a return value and need to preserve whether it
   was a reference.
6. No. It hides that the result is unsigned, which is exactly the trap in
   section 2.

</details>

---

[next: capstone, rebuild ImageContent →](10-capstone-imagecontent.md)
