[← strong types](03-strong-types.md) · [chapter index](README.md) · [next: initialisation →](05-initialisation.md)

# 4. Value categories

**Time:** 60 minutes
**Code:** [`code/04_value_categories.cpp`](code/04_value_categories.cpp)

```sh
cd code && make 04_value_categories && ./04_value_categories
```

Every expression in C++ has a type *and* a value category. The category
decides which overload gets picked, whether you can bind a reference to
it, and whether the compiler is allowed to steal its guts. Most confusion
about `std::move` is really confusion about categories.

---

## how to see a category

You can't print a value category. But you can make the compiler tell you,
by overloading on it:

```cpp
void cat(int&)  { std::puts("lvalue"); }
void cat(int&&) { std::puts("rvalue"); }
```

`int&` binds only to lvalues. `int&&` binds only to rvalues. Whichever one
gets called tells you the category of the argument expression. That trick
is used in every demo below, and it's worth keeping in your own toolbox.

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
the two questions:

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

```
-- a named rvalue reference is an LVALUE --
  param declared int&&, but `r` itself   -> lvalue
  std::move(r)                           -> rvalue
```

Read that twice.

```cpp
void inside(int&& r) {
    cat(r);              // prints "lvalue"
}
```

The parameter's *type* is `int&&`. But `r` is a name, and naming something
makes the expression an lvalue. Types and categories are different axes.

**Why it has to be this way:** if `r` stayed an rvalue inside the
function, then

```cpp
void f(std::string&& s) {
    use(s);      // if this moved from s...
    use(s);      // ...this would see a gutted string
}
```

would be a disaster. Making the name an lvalue means you must *ask* to
move, explicitly, and you can see where you asked.

**The consequence:** when you forward an `&&` parameter onward, you must
re-`move` it.

```cpp
void take(std::string&& s) {
    store(std::move(s));    // the std::move is REQUIRED
}
```

Drop the `std::move` and you silently get a copy.

---

## `std::move` moves nothing

```
-- std::move does not move --
after a bare std::move(a):  a = 'payload'   <- untouched
after b = std::move(a):     a = ''  b = 'payload'
std::move only changes the expression's category. the actual
stealing is done by whatever constructor or assignment runs.
```

`std::move` is a cast. That's the whole implementation:

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
//   so string's move constructor is chosen instead of the copy ctor
//   and THAT is what steals a's buffer
```

`std::move` on its own line does nothing at all, which is why it's
`[[nodiscard]]` and why the demo has to `(void)` it to keep the build
clean.

**Better name:** `rvalue_cast`. It was proposed. It lost. We live with
`move`.

---

## what's left behind

```
-- what is left behind --
dst        = 'a fairly long string to defeat SSO buffers'
src.size() = 0   (valid object, unspecified value)
src        = 'reassigned'   <- assigning is always fine
you may destroy or assign a moved-from object. do not READ it.
```

The standard says a moved-from standard library object is in a **valid but
unspecified state**. Two words, both important:

- **valid** — the invariants hold. Destroying it is fine. Assigning to it
  is fine. Calling any operation with no precondition is fine
  (`size()`, `empty()`, `clear()`).
- **unspecified** — you don't get to know *what* value. libstdc++ leaves
  a moved-from string empty; MSVC might too; the standard doesn't promise
  it and a future version could change.

So:

```cpp
std::string b = std::move(a);

a.clear();          // fine
a = "new value";    // fine
a.size();           // fine, but the answer is not guaranteed to be 0
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

`const T&` binding to everything is why it's the default for a read-only
parameter.

### 3. elision

A prvalue returned from a function isn't a temporary that gets copied — in
C++17 it's *constructed directly* into the destination. Section 8 has the
proof.

---

## `decltype` and the parens

One place categories leak into type deduction:

```cpp
int x = 0;
decltype(x)    // int      -- x is a declared name, gives the declared type
decltype((x))  // int&     -- (x) is an expression, and it's an lvalue
```

`decltype` on a name gives the declared type. `decltype` on an
*expression* gives the type adjusted by the category: `T&` for lvalues,
`T&&` for xvalues, `T` for prvalues. Wrapping in parens turns the name
into an expression.

This is a real gotcha in return types. Section 9 covers
`decltype(auto)`.

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

1. `cat(int&)` then `cat(int&&)`. `x` is an lvalue, `std::move(x)` is an
   xvalue which is an rvalue.
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
