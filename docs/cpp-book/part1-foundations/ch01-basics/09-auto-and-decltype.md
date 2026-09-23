[← copy, move, elision](08-copy-move-elision.md) · [chapter index](README.md) · [next: capstone →](10-capstone-imagecontent.md)

# 9. auto and decltype

**Time:** 45 minutes
**Code:** [`code/09_auto.cpp`](code/09_auto.cpp)

```sh
cd code && make 09_auto && ./09_auto
```

---

## auto drops references and top-level const

```
-- auto strips ref and top-level const --
after writing to the autos: x = 1, c = 2
is_same<decltype(a1), int> = true
through auto& : x = 77
const auto& view = 77
```

```cpp
int        x = 1;
int&       r = x;
const int  c = 2;
const int& cr = c;

auto a1 = r;    // int  -- NOT int&
auto a2 = c;    // int  -- NOT const int
auto a3 = cr;   // int  -- both dropped
```

`auto` deduces like a by-value template parameter: references are
stripped, and top-level `const` is stripped. You get a fresh, writable
copy.

The output proves it — writing 9 to all three autos left `x` and `c`
untouched.

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

`auto` drops the const *on the thing being copied*. `const int*` is "a
pointer to const int" — the pointer itself isn't const, so nothing is
dropped. `int* const` is "a const pointer", and copying it gives you a
writable pointer.

---

## the range-for copy

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

```
-- the map trap --
 value_type is pair<const string, int>, NOT pair<string,int>.
 so `for (const std::pair<std::string,int>& kv : m)` copies
 every element to convert the type. use auto&:
   a -> 1
   b -> 2
```

This one is nasty because you wrote a `const&` and still got a copy.

```cpp
std::map<std::string, int> m;

for (const std::pair<std::string, int>& kv : m)    // COPIES EVERY ELEMENT
```

`std::map<K,V>::value_type` is `std::pair<const K, V>`. The `K` is const,
because changing a key in place would break the tree ordering.

You wrote `pair<string, int>`, which is a *different type*. A `const&`
can't bind to a different type, so the compiler creates a temporary of
your type, converts, binds to that, and destroys it each iteration. Silent
copy of every key and value.

```cpp
for (const auto& [k, v] : m)     // correct, and shorter
```

`auto` deduces the real `value_type` and no conversion happens. Structured
bindings on top make it readable.

**This is the strongest practical argument for `auto` in range-for:** the
type is easy to get subtly wrong, and being wrong costs a copy silently.

---

## decltype

```
-- decltype --
decltype(x)    is int   : yes
decltype((x))  is int&  : yes   <- extra parens matter
decltype(val()) is int  : yes
decltype(ref()) is int& : yes
after d = 42: ref() = 42, a = 5
```

`decltype` gives you the *exact* declared type, keeping references and
const. Unlike `auto`, it strips nothing.

```cpp
int  x = 0;
int& r = x;

decltype(x)  a;     // int
decltype(r)  b = x; // int&    -- reference kept
```

### the parens rule

```cpp
decltype(x)    // int   -- x is a NAME, gives the declared type
decltype((x))  // int&  -- (x) is an EXPRESSION, and it's an lvalue
```

For an expression (not a plain name), `decltype` gives:

- `T&` if it's an lvalue
- `T&&` if it's an xvalue
- `T` if it's a prvalue

Wrapping a name in parens makes it an expression. So `decltype((x))` is
`int&`.

This is the classic gotcha:

```cpp
decltype(auto) f() {
    int x = 0;
    return (x);     // deduces int&. returns a reference to a dead local.
}
```

### `decltype(auto)`

```cpp
int& ref() { static int n = 5; return n; }

auto           a = ref();   // int   -- reference dropped, a is a copy
decltype(auto) d = ref();   // int&  -- reference kept, d aliases n
d = 42;                     // changes n
```

`decltype(auto)` means "deduce, but use `decltype` rules". It's for
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

### the `auto x = T{...}` style

Some people write everything as `auto x = Type{args};`. It guarantees
initialisation (no most vexing parse), puts the type on the right where
it's easy to change, and lines up nicely. It's a defensible style. Just
pick one and be consistent — agentty uses plain `auto` where the type is
obvious and explicit types elsewhere.

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
