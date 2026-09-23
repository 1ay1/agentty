[← strong types](03-strong-types.md) · [chapter index](README.md) · [next: initialisation →](05-initialisation.md)

# 4. Value categories

**Time:** 60 minutes. Type everything.

Every expression in C++ has a type *and* a value category. The category
decides which overload gets picked, whether you can bind a reference to
it, and whether the compiler is allowed to steal its guts.

Most confusion about `std::move` is really confusion about categories.
We're going to make categories **visible**, which is the only way to stop
guessing about them.

Open `04_value_categories.cpp`.

---

## build the detector

You can't print a value category. But you can make the compiler tell you,
by overloading on it:

```cpp
#include <cstdio>
#include <string>
#include <utility>

static void cat(int&)  { std::puts("lvalue"); }
static void cat(int&&) { std::puts("rvalue"); }
```

That's the whole trick, and it's worth keeping in your own toolbox.

`int&` binds **only** to lvalues. `int&&` binds **only** to rvalues. So
whichever overload gets called tells you the category of whatever you
passed. The compiler does the classification; you just read the output.

Now use it:

```cpp
static int  global = 100;
static int& give_lvalue()  { return global; }   // returns a reference
static int  give_prvalue() { return 42; }       // returns a value

static void categories() {
    std::puts("-- what kind of expression is this? --");
    int x = 1;
    int arr[3]{};

    std::printf("x                 -> "); cat(x);
    std::printf("42                -> "); cat(42);
    std::printf("x + 1             -> "); cat(x + 1);
    std::printf("arr[0]            -> "); cat(arr[0]);
    std::printf("give_lvalue()     -> "); cat(give_lvalue());
    std::printf("give_prvalue()    -> "); cat(give_prvalue());
    std::printf("std::move(x)      -> "); cat(std::move(x));
}

int main() {
    categories();
}
```

**Predict each line before you run it.** Then run:

```
-- what kind of expression is this? --
x                 -> lvalue
42                -> rvalue
x + 1             -> rvalue
arr[0]            -> lvalue
give_lvalue()     -> lvalue
give_prvalue()    -> rvalue
std::move(x)      -> rvalue
```

The two that trip people up: `arr[0]` is an **lvalue** (it names a real
element that persists), and `x + 1` is an **rvalue** (the sum is a value
that exists only for this expression).

---

## the taxonomy

Formally there are five, arranged in a lattice:

```
            expression
           /          \
      glvalue         rvalue
      /     \         /     \
  lvalue     xvalue        prvalue
```

- **lvalue** — names an object that persists. Has identity. You can take
  its address. `x`, `arr[0]`, `*ptr`, `obj.member`, a function returning
  `T&`.
- **prvalue** — a pure value, not yet an object. `42`, `x + 1`, a function
  returning `T` by value, `Foo{}`.
- **xvalue** — an "eXpiring" value. Names an object, *and* you're allowed
  to gut it. `std::move(x)`, a function returning `T&&`.
- **glvalue** — lvalue or xvalue. "has identity".
- **rvalue** — prvalue or xvalue. "can be moved from".

You will never need to say "glvalue" out loud. What you need day to day is
two questions:

**Does it have identity?** (can I take its address, will it still be there
on the next line?) → lvalue-ish.
**Can I steal from it?** → rvalue-ish.

| | has identity | can be moved from |
|---|---|---|
| lvalue | yes | no |
| xvalue | yes | yes |
| prvalue | no | yes |

---

## the trap everyone hits

Add this:

```cpp
static void inside(int&& r) {
    std::printf("  param declared int&&, but `r` itself   -> "); cat(r);
    std::printf("  std::move(r)                           -> "); cat(std::move(r));
}

static void named_rvalue_ref() {
    std::puts("\n-- a named rvalue reference is an LVALUE --");
    inside(42);
}
```

```
-- a named rvalue reference is an LVALUE --
  param declared int&&, but `r` itself   -> lvalue
  std::move(r)                           -> rvalue
```

**Read that twice.**

The parameter's *type* is `int&&`. But `r` is a **name**, and naming
something makes the expression an lvalue. Types and categories are
different axes.

### why it has to be this way

If `r` stayed an rvalue inside the function, then:

```cpp
void f(std::string&& s) {
    use(s);      // if this silently moved from s...
    use(s);      // ...this would see a gutted string
}
```

would be a disaster. Making the name an lvalue means you must **ask** to
move, explicitly, and the `std::move` is visible in the code where you
asked.

### the consequence

When you forward an `&&` parameter onward, you must re-`move` it:

```cpp
void take(std::string&& s) {
    store(std::move(s));    // the std::move is REQUIRED
}
```

Drop the `std::move` there and you silently get a copy. This is one of the
most common performance bugs in C++, and it produces no warning.

---

## `std::move` moves nothing

```cpp
static void move_is_a_cast() {
    std::puts("\n-- std::move does not move --");
    std::string a = "payload";

    (void)std::move(a);       // a bare std::move, result discarded
    std::printf("after a bare std::move(a):  a = '%s'   <- untouched\n",
                a.c_str());

    std::string b = std::move(a);   // NOW the move constructor runs
    std::printf("after b = std::move(a):     a = '%s'  b = '%s'\n",
                a.c_str(), b.c_str());
}
```

```
-- std::move does not move --
after a bare std::move(a):  a = 'payload'   <- untouched
after b = std::move(a):     a = ''  b = 'payload'
```

`std::move` is a **cast**. That's the entire implementation:

```cpp
template <typename T>
constexpr std::remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}
```

It produces no code. It changes nothing at runtime. It takes an expression
and hands you back an xvalue version of it.

The *actual* move happens because that xvalue then selects a different
overload:

```cpp
std::string b = std::move(a);
//              ^^^^^^^^^^^^^ now an xvalue
//   so string's MOVE constructor is chosen instead of the copy ctor,
//   and THAT is what steals a's buffer
```

`std::move` on its own line does nothing at all. Which is why it's
`[[nodiscard]]` — try removing the `(void)` and see:

```
warning: ignoring return value of 'constexpr typename
std::remove_reference<_Tp>::type&& std::move(_Tp&&)', declared with
attribute 'nodiscard' [-Wunused-result]
```

> **Better name:** `rvalue_cast`. It was proposed. It lost. We live with
> `move`.

---

## what's left behind

```cpp
static void moved_from_state() {
    std::puts("\n-- what is left behind --");
    std::string src = "a fairly long string to defeat SSO buffers";
    std::string dst = std::move(src);
    std::printf("dst        = '%s'\n", dst.c_str());
    std::printf("src.size() = %zu   (valid object, unspecified value)\n",
                src.size());
    src = "reassigned";            // always legal
    std::printf("src        = '%s'   <- assigning is always fine\n",
                src.c_str());
}
```

```
-- what is left behind --
dst        = 'a fairly long string to defeat SSO buffers'
src.size() = 0   (valid object, unspecified value)
src        = 'reassigned'   <- assigning is always fine
```

Note the deliberately **long** string. A short one might live entirely in
the SSO buffer (section 1) with no heap allocation to steal, and then the
"move" is just a copy and the demo proves nothing.

The standard says a moved-from standard library object is in a **valid but
unspecified state**. Two words, both load-bearing:

- **valid** — the invariants hold. Destroying it is fine. Assigning to it
  is fine. Calling any operation with no precondition is fine (`size()`,
  `empty()`, `clear()`).
- **unspecified** — you don't get to know *what* value. libstdc++ leaves a
  moved-from string empty; the standard doesn't promise it, and a future
  version could change.

So:

```cpp
std::string b = std::move(a);

a.clear();          // fine
a = "new value";    // fine
a.size();           // fine, but the answer isn't guaranteed to be 0
std::cout << a;     // legal but MEANINGLESS. don't.
a.front();          // UB if a happens to be empty. precondition violated.
```

**Practical rule: after you move from something, either assign to it or
let it die. Don't read it.**

For your own types, decide and document. agentty's convention is the
standard one: moved-from means "empty and reusable".

---

## where categories actually matter

### 1. picking an overload

```cpp
void store(const std::string& s);   // copies
void store(std::string&& s);        // moves

store(name);               // lvalue  -> copies
store(std::move(name));    // xvalue  -> moves
store(compute_name());     // prvalue -> moves
```

One function name, and the compiler routes to the cheap version whenever
it can prove nobody else needs the value.

### 2. what a reference can bind

Write these and read each error:

```cpp
std::string s = "x";

std::string&       a = s;                // ok: lvalue ref to lvalue
std::string&       b = std::string{"x"}; // ERROR: can't bind to rvalue
const std::string& c = std::string{"x"}; // ok: const& binds anything
std::string&&      d = std::string{"x"}; // ok: rvalue ref to prvalue
std::string&&      e = s;                // ERROR: can't bind to lvalue
```

That table is worth memorising:

| binds to | lvalue | rvalue |
|----------|--------|--------|
| `T&` | yes | no |
| `const T&` | yes | yes |
| `T&&` | no | yes |

`const T&` binding to everything is exactly why it's the default for a
read-only parameter.

### 3. elision

A prvalue returned from a function isn't a temporary that gets copied — in
C++17 it's *constructed directly* into the destination. Section 8 proves
it.

---

## `decltype` and the parens

One place categories leak into type deduction:

```cpp
int x = 0;
decltype(x)    // int      -- x is a declared NAME, gives the declared type
decltype((x))  // int&     -- (x) is an EXPRESSION, and it's an lvalue
```

`decltype` on a name gives the declared type. `decltype` on an
*expression* gives the type adjusted by the category: `T&` for lvalues,
`T&&` for xvalues, `T` for prvalues. Wrapping in parens turns the name
into an expression.

This is a real gotcha in return types. Section 9 covers `decltype(auto)`.

---

## the whole file

```cpp
// 04_value_categories.cpp — lvalue, prvalue, xvalue, and what std::move is.

#include <cstdio>
#include <string>
#include <utility>

// Two overloads. Which one the compiler picks tells you the value
// category of the argument expression.
static void cat(int&)  { std::puts("lvalue"); }
static void cat(int&&) { std::puts("rvalue"); }

// ── the categories ─────────────────────────────────────────────────────
static int  global = 100;
static int& give_lvalue() { return global; }   // returns a reference
static int  give_prvalue() { return 42; }      // returns a value

static void categories() {
    std::puts("-- what kind of expression is this? --");
    int x = 1;
    int arr[3]{};

    std::printf("x                 -> "); cat(x);
    std::printf("42                -> "); cat(42);
    std::printf("x + 1             -> "); cat(x + 1);
    std::printf("arr[0]            -> "); cat(arr[0]);
    std::printf("give_lvalue()     -> "); cat(give_lvalue());
    std::printf("give_prvalue()    -> "); cat(give_prvalue());
    std::printf("std::move(x)      -> "); cat(std::move(x));
    std::puts("\nrule of thumb: can you take its address and will it still");
    std::puts("be there next line? then it is an lvalue.");
}

// ── the trap: a named rvalue reference is an lvalue ────────────────────
static void inside(int&& r) {
    std::printf("  param declared int&&, but `r` itself   -> "); cat(r);
    std::printf("  std::move(r)                           -> "); cat(std::move(r));
}

static void named_rvalue_ref() {
    std::puts("\n-- a named rvalue reference is an LVALUE --");
    inside(42);
    std::puts("  this is why you still write std::move(x) when forwarding");
    std::puts("  an int&& parameter onward. the name makes it an lvalue again.");
}

// ── std::move is a cast. it moves nothing. ─────────────────────────────
static void move_is_a_cast() {
    std::puts("\n-- std::move does not move --");
    std::string a = "payload";

    // INTENTIONAL: std::move is [[nodiscard]]; discarding it is precisely
    // the mistake this demo exists to show.
    (void)std::move(a);
    std::printf("after a bare std::move(a):  a = '%s'   <- untouched\n",
                a.c_str());

    std::string b = std::move(a);   // NOW the move constructor runs
    std::printf("after b = std::move(a):     a = '%s'  b = '%s'\n",
                a.c_str(), b.c_str());
    std::puts("std::move only changes the expression's category. the actual");
    std::puts("stealing is done by whatever constructor or assignment runs.");
}

// ── moved-from is valid but unspecified ────────────────────────────────
static void moved_from_state() {
    std::puts("\n-- what is left behind --");
    std::string src = "a fairly long string to defeat SSO buffers";
    std::string dst = std::move(src);
    std::printf("dst        = '%s'\n", dst.c_str());
    std::printf("src.size() = %zu   (valid object, unspecified value)\n",
                src.size());
    src = "reassigned";            // always legal
    std::printf("src        = '%s'   <- assigning is always fine\n",
                src.c_str());
    std::puts("you may destroy or assign a moved-from object. do not READ it.");
}

int main() {
    categories();
    named_rvalue_ref();
    move_is_a_cast();
    moved_from_state();
}
```

---

## now break it

1. Add `static void cat(const int&)` as a third overload. Which calls
   change? (Hint: `const int&` binds to both, so it only wins when
   neither exact match applies.)
2. Remove the `(void)` from the bare `std::move` and read the
   `-Wunused-result` warning.
3. In `inside`, remove the `std::move` from the second line. Watch it
   print `lvalue` both times.
4. Change the long string in `moved_from_state` to `"hi"`. Does `src`
   still end up empty? Why might it not? (SSO — section 1.)
5. Write all five reference bindings from the table above and read the
   two errors.

---

## check yourself

1. `int x; cat(x);` and `cat(std::move(x));` — which overload each, and
   why?
2. Inside `void f(std::string&& s)`, is `s` an lvalue or an rvalue?
3. What does `std::move` do at runtime?
4. After `std::string b = std::move(a);`, which of these is fine:
   `a.clear()`, `a = "x"`, `std::cout << a`, `a.front()`?
5. Why is `const T&` able to bind to a temporary when `T&` isn't?

<details>
<summary>answers</summary>

1. `cat(int&)` then `cat(int&&)`. `x` is an lvalue; `std::move(x)` is an
   xvalue, which is an rvalue.
2. lvalue. The type is `std::string&&` but the expression `s` names
   something, so it's an lvalue.
3. Nothing. It's a `static_cast` to an rvalue reference and generates no
   instructions.
4. `a.clear()` fine, `a = "x"` fine, `std::cout << a` legal but the value
   is unspecified so it's meaningless, `a.front()` is UB if empty.
5. Because binding a non-const `T&` to a temporary would let you modify an
   object that's about to be destroyed, which is almost always a bug. A
   `const T&` can't modify it, and the language extends the temporary's
   lifetime to match the reference.

</details>

---

[next: initialisation →](05-initialisation.md)
