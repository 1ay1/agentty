# Chapter 1: Types, Values, and References

**Goal:** Understand what a C++ object *is*, how names refer to it, when it dies, and why agentty's type system catches bugs that other languages discover at 3am in production.

**Time:** 6–10 hours. This is the longest chapter in the book, deliberately — everything later stands on it.

**Prerequisites:** You can write a loop and call a function in *some* language.

---

## How to read this chapter

Every example here is real, compiled code. The companion file `ch01_examples.cpp` sits next to this README and contains all of them:

```sh
cd docs/cpp-book/part1-foundations/ch01-basics
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined -g \
    ch01_examples.cpp -o ch01 && ./ch01
```

Every output block below is the **actual output** of that program, not something I typed by hand. Run it and follow along.

Blocks marked `// ERROR:` are *supposed* to fail. Compile them anyway and read what your compiler says — learning to read compiler errors is half of learning C++.

Alongside each concept you'll find **the real agentty code that uses it**, with a file path. By the end of this chapter you'll understand `include/agentty/domain/id.hpp` and `include/agentty/domain/lazy_bytes.hpp` completely.

---

## 1.0 The mental model you need first

Most languages let you stay vague about one question: **where does this value live, and who destroys it?** Python, Java, Go and JavaScript all answer "somewhere on the heap, and the garbage collector handles it eventually."

C++ makes you answer it. That is the entire difficulty of the language, and the entire source of its speed. Four sentences, and most of C++ stops being arbitrary:

1. An **object** is a region of storage with a type and a lifetime.
2. A **name** is not an object — it's a way to reach one.
3. Every object has a well-defined moment it is **destroyed**, usually determined by the scope it was created in.
4. Copying is explicit, moving is explicit, and both are visible in the type system.

If you read only one section, read **§1.8 (lifetime)**. That's where the real bugs live.

---

## 1.1 What is a type, really?

A type is three things at once:

- **A size and layout** — how many bytes, arranged how
- **A set of operations** — what you're allowed to do
- **A set of promises** — what the compiler will enforce

```
=== 1.1 what a type is ===
int 4  double 8  char 1 bytes
```

All three can hold "42", but the *bits* differ completely. `int` 42 is `00000000 00000000 00000000 00101010`. `double` 42.0 is an IEEE-754 sign/exponent/mantissa arrangement sharing no bits with it. The type tells the compiler how to interpret the storage.

### Types constrain operations

```cpp
int a = 10, b = 3;
std::string s = "hello", t = "world";

int q = a / b;             // 3 — integer division
// auto bad = s / t;       // ERROR: no operator/ for std::string
```

```
error: no match for 'operator/' (operand types are 'std::string' and 'std::string')
```

The compiler isn't being difficult. Division of two strings has no meaning, so the language refuses to guess.

### Integer division is the first trap

```
7 / 2        = 3   <- integer division truncates
7.0 / 2.0    = 3.5
(double)7/2  = 3.5
```

`7 / 2` is `3`, not `3.5`. Both operands are `int`, so the compiler picks integer division. It truncates toward zero and does **not** warn you, because this is well-defined and sometimes exactly what you want.

This causes real bugs. Computing a percentage:

```cpp
int done = 3, total = 4;
int    wrong = (done / total) * 100;                           // 0
double right = (static_cast<double>(done) / total) * 100.0;    // 75
```

```
progress wrong: 0%
progress right: 75%
```

`done / total` is `3/4` = `0` in integer arithmetic, then `0 * 100` = `0`. Your progress bar reads 0% until the job finishes, then jumps to 100%.

---

## 1.2 Integers: the part everyone gets wrong

C++ inherits C's integer rules, designed for 1972 hardware. Three edges you must know.

### Trap 1: signed overflow is undefined behaviour

```cpp
int max = std::numeric_limits<int>::max();   // 2147483647
int bad = max + 1;                            // UNDEFINED BEHAVIOUR
```

This might print `-2147483648`. It might print `2147483647`. With optimisation on it might delete your loop entirely. **Undefined behaviour means the compiler may assume it never happens** and optimise accordingly.

Watch it happen:

```cpp
// overflow.cpp
#include <cstdio>
bool always_true(int x) {
    return x + 1 > x;   // compiler reasons: signed overflow is UB,
}                       // therefore this is ALWAYS true
int main() { std::printf("%d\n", always_true(2147483647)); }
```

```sh
$ g++ -O0 overflow.cpp -o a && ./a
0                    # actually computed, wrapped around
$ g++ -O2 overflow.cpp -o a && ./a
1                    # whole function replaced with `return true`
```

**The same program gives different answers at different optimisation levels.** That's UB — not "unspecified", not "platform-dependent", but a promise you made to the compiler and broke.

Catch it:

```sh
g++ -std=c++23 -fsanitize=undefined -g overflow.cpp -o a && ./a
# runtime error: signed integer overflow: 2147483647 + 1 cannot be
#   represented in type 'int'
```

**Use `-fsanitize=undefined` while learning. Always.** It turns silent corruption into a clear message.

### Trap 2: unsigned wraps — silently and legally

```
empty.size() - 1 = 18446744073709551615   <- wrapped!
```

`std::vector::size()` returns `std::size_t`, which is unsigned. On an empty vector, `size() - 1` wraps to 18 quintillion. This loop reads far past the end:

```cpp
for (std::size_t i = 0; i < v.size() - 1; ++i)   // BUG on empty v
```

Unsigned overflow is *defined* to wrap, so no sanitizer catches it.

**The fix is to never subtract.** Move the term to the other side:

```cpp
for (std::size_t i = 0; i + 1 < v.size(); ++i)
    std::printf("(%d,%d) ", v[i], v[i + 1]);
```

```
adjacent pairs: (10,20) (20,30) 
```

Safe on an empty vector, because `0 + 1 < 0` is simply false.

### Trap 3: comparing signed with unsigned

```
s < u           : false  <- wrong      (s = -1, u = 1)
std::cmp_less   : true   <- right
```

`s` gets converted to unsigned, becoming 4294967295. `-Wall -Wextra` warns:

```
warning: comparison of integer expressions of different signedness
```

C++20 gave us a real fix: `std::cmp_less`, `cmp_greater`, `cmp_equal` compare **mathematical values**, not bit patterns.

### Practical rules

- `int` for ordinary arithmetic and small loop counters
- `std::size_t` for sizes and indices — never subtract without a guard
- `std::int64_t`, `std::uint32_t` (from `<cstdint>`) when width matters: serialisation, protocols, file formats
- `-Wall -Wextra -fsanitize=undefined,address` on every practice program

---

## 1.3 Strong types — agentty's `Id<Tag>`

Here's a bug that actually shipped:

```cpp
struct Request { std::string provider_id, model_id; };

std::string fetch_model_id(const Request& r) {
    return r.provider_id;   // BUG: wrong field, same type, compiles fine
}
```

Both fields are `std::string`, so the compiler has nothing to object to. The user picks GPT-4, gets something else, no error anywhere. Found weeks later by someone reading logs.

The type system could have caught this, if we'd let it.

### The real agentty code

Open `include/agentty/domain/id.hpp`. Here is the actual type, in full:

```cpp
template <typename Tag>
struct Id {
    std::string value;

    Id() = default;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}

    [[nodiscard]] bool        empty() const noexcept { return value.empty(); }
    [[nodiscard]] const char* c_str() const noexcept { return value.c_str(); }

    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;

    [[nodiscard]] bool operator==(std::string_view sv) const noexcept {
        return value == sv;
    }

    friend void to_json(nlohmann::json& j, const Id& id) { j = id.value; }
    friend void from_json(const nlohmann::json& j, Id& id) { j.get_to(id.value); }
};

struct ThreadIdTag     {};
struct ToolCallIdTag   {};
struct ModelIdTag      {};

using ThreadId   = Id<ThreadIdTag>;
using ToolCallId = Id<ToolCallIdTag>;
using ModelId    = Id<ModelIdTag>;
```

Every piece of that earns its place. Let's go through it.

**`template <typename Tag>`** — `Tag` is never used in the body. It exists purely to make `Id<ThreadIdTag>` and `Id<ToolCallIdTag>` *different types*. This is the "phantom type" pattern.

```
=== 1.3 strong types ===
sizeof(std::string) = 32
sizeof(ThreadId)    = 32  <- identical
```

Same size, same generated code. The wrapper exists only during compilation. This is **zero-overhead abstraction** — the central promise of C++ and the theme of this book.

**`explicit`** — without it, any `std::string` silently becomes a `ThreadId` and you're back where you started:

```cpp
void save(ThreadId id);
save("abc123");          // ERROR: explicit blocks the implicit conversion
save(ThreadId{"abc123"}); // must say what you mean
```

**`noexcept`** — this constructor cannot throw, because `std::move` on a `std::string` only steals a pointer. Chapter 4 shows why that keyword makes `std::vector<ThreadId>` measurably faster.

**`[[nodiscard]]`** — calling `id.empty()` and ignoring the answer is always a bug. The attribute makes it a warning:

```cpp
id.empty();              // warning: ignoring return value
if (id.empty()) { ... }  // fine
```

**`= default` on `operator==`** — C++20 generates the obvious member-wise comparison. You get `==`, `!=`, and from `<=>` all four relational operators, without writing them.

**`friend void to_json(...)`** — found by argument-dependent lookup. When nlohmann's JSON library serialises a `ThreadId`, it finds this function because it's declared in the same namespace as the type. Chapter 3 covers ADL properly.

### What it prevents

```cpp
ThreadId   t{"abc123"};
ToolCallId c{"abc123"};

// t == c;        // ERROR: no operator== for these two types
// save_thread(c); // ERROR: cannot convert ToolCallId to ThreadId
```

The bug is now **impossible to write**. Not discouraged by convention, not caught in review — mechanically impossible, checked on every build.

---

## 1.4 Value categories: lvalue, prvalue, xvalue

This is the concept people skip, and then move semantics never makes sense. Twenty minutes. Do it now.

Every *expression* has two independent properties: its **type**, and its **value category**.

| Category | Informally | Has a name? | Address? |
|---|---|---|---|
| **lvalue** | a thing with identity | yes | yes |
| **prvalue** | a fresh value, no identity | no | no |
| **xvalue** | a thing being emptied out | yes | yes |

The memorable rule: **if it has a name, it's an lvalue.**

### Proving it with overloads

```cpp
void cat(int&)  { std::puts("lvalue"); }
void cat(int&&) { std::puts("rvalue"); }

int x = 1;
cat(x);              // lvalue
cat(42);             // rvalue
cat(x + 1);          // rvalue — x+1 makes a new temporary
cat(std::move(x));   // rvalue
```

```
cat(x)             ->   lvalue
cat(42)            ->   rvalue
cat(x + 1)         ->   rvalue
cat(std::move(x))  ->   rvalue
```

### `std::move` does not move anything

The most misleading name in the standard library:

```cpp
std::string a = "data";
(void)std::move(a);          // NOTHING HAPPENS
std::string b = std::move(a); // NOW it moves
```

```
after std::move(a) alone: a='data'
after b = std::move(a):   a='' b='data'
```

`std::move(a)` is a **cast**. It produces an xvalue and compiles to zero instructions. The *move* happens when something consumes that xvalue — here, `b`'s constructor.

If it were named `std::rvalue_cast`, a decade of confusion would have been avoided.

### The trap that catches everyone

```cpp
void f(int&& r) {
    cat(r);              // lvalue!  r has a NAME
    cat(std::move(r));   // rvalue   must re-cast
}
```

```
  inside f(int&& r), the expression `r` is an:   lvalue
  after std::move(r):                            rvalue
```

**A named rvalue reference is an lvalue.** This is deliberate safety — because `r` has a name you could use it again after moving, so the compiler makes you state your intent.

---

## 1.5 Initialisation: five ways, and which to use

```
int b{}=0  c{5}=5  d(5)=5  e=5 -> 5
Point p1{}=(0,0)  p2{1,2}=(1,2)
```

### Why `{}` is the better default: narrowing

```cpp
double d = 3.99;
int a(d);       // 3 — silently truncates
int b = d;      // 3 — silently truncates
// int c{d};    // ERROR: narrowing conversion
```

```
int(3.99) via () = 3   <- silent truncation
uint8_t = 300 via = : 44   <- silently wrapped
```

300 doesn't fit in a byte. Parentheses shrug and give you 44. **Braces refuse to compile.** A bug caught for free, at compile time, by typing two different characters.

### The one place `{}` surprises you

```
vector(5,0).size()=5   vector{5,0}.size()=2
```

`std::vector<int> a(5, 0)` is five zeros. `std::vector<int> b{5, 0}` is two elements: 5 and 0. `std::initializer_list` constructors win over everything when you use braces.

### Most vexing parse

```cpp
Widget w1();     // NOT a variable — declares a FUNCTION taking nothing
Widget w2{};     // an actual variable
```

Anything that *can* be parsed as a declaration *is*. `Widget w1()` looks exactly like a function prototype, so that's what it becomes. Braces are immune.

### Summary

| Form | Meaning | Use when |
|---|---|---|
| `T x;` | default-init (garbage for scalars) | basically never |
| `T x{};` | value-init (zeroed) | **default choice** |
| `T x{a,b};` | list-init, narrowing is an error | **default choice** |
| `T x(a,b);` | direct-init | ctor takes a count/size |
| `T x = a;` | copy-init | rarely |

**Rule: use `{}` unless the type has a count-like constructor.**

---

## 1.6 References

A reference is an **alias**. Not an object, no storage of its own, can never be reseated.

```cpp
int x = 1, y = 2;
int& r = x;      // r IS x
r = y;           // assigns y's VALUE into x. Does NOT rebind r.
```

```
after r = y: x=2 y=2 r=2
&x == &r ? yes  <- r IS x
```

Same address. They are the same object under two names.

### A reference can never be null

```cpp
// int& r;        // ERROR: no unbound references
int* p = nullptr; // fine — pointers can be nothing
```

This shows up in agentty's signatures as documentation. From `src/acp/server.cpp`:

```cpp
const Model* find(const ModelId& id) const;   // returns nullptr if absent
void must_have(const Config& c);              // caller guarantees it exists
```

The signature *is* the contract. A reader knows instantly whether to expect null.

### `const&` binds everything

```cpp
void take(const std::string& s);

std::string a = "lvalue";
take(a);              // lvalue
take("temporary");    // prvalue — temporary created, lives to end of statement
take(std::move(a));   // xvalue
```

That's why `const T&` is the default for read-only parameters: accepts every category, copies nothing.

### …but not for small types

A reference is a pointer underneath. For a 4-byte `int`, passing a reference costs an indirection to avoid copying four bytes.

```cpp
void f(int x);                  // yes
void f(const int& x);           // pointless, possibly slower
void f(std::string_view s);     // yes — 16 bytes, no allocation
void f(const std::string& s);   // yes, when you need std::string itself
```

**Rule of thumb:** by value if `sizeof(T) <= 16` and trivially copyable, otherwise `const&`.

### Reference collapsing

You can't *write* a reference to a reference, but a template can *produce* one:

```
T&  &   →  T&
T&  &&  →  T&
T&& &   →  T&
T&& &&  →  T&&
```

Only `&& &&` stays `&&`. **Any lvalue reference wins.** This is the machinery behind perfect forwarding (Chapter 3).

---

## 1.7 `const` — and agentty's `mutable` cache

`const` means "not modifiable *through this name*". A compile-time promise, not a runtime lock.

### Top-level vs low-level

```cpp
const int* p1 = &x;        // pointer to const int   — LOW-LEVEL
// *p1 = 5;                // ERROR: can't modify pointee
p1 = &y;                   // fine: pointer is mutable

int* const p2 = &x;        // const pointer to int   — TOP-LEVEL
*p2 = 5;                   // fine: pointee is mutable
// p2 = &y;                // ERROR: can't repoint

const int* const p3 = &x;  // both
```

**Read pointer declarations right-to-left.** `const int* p1` → "p1 is a pointer to an int that is const".

### `const` member functions

A `const T&` can only call `const` member functions. That's how read-only access is enforced.

### The `mutable` escape hatch — a real agentty use

Now the interesting part. Open `include/agentty/domain/lazy_bytes.hpp`.

agentty stores images and tool outputs as content-addressed blobs on disk. A `Message` holding an image shouldn't load the bytes until something actually needs them. But `bytes()` is a *read* — it should be callable on a `const Message`.

The conflict: reading requires filling a cache, and filling a cache is a mutation.

```cpp
class LazyBytes {
public:
    struct Source { std::string blob; };

    [[nodiscard]] bool materialised() const noexcept { return have_; }

    // const, yet it fills the cache — that is what `mutable` is for.
    [[nodiscard]] const std::string& bytes() const {
        if (!have_) {
            cache_ = resolve_from_disk(src_);   // logically a read
            have_  = true;
        }
        return cache_;
    }

private:
    Source              src_;
    mutable std::string cache_;
    mutable bool        have_ = false;
};
```

```
=== 1.7 const + mutable cache ===
  materialised? no
    (resolving blob 'sha256-abc' from disk...)
  first  bytes(): BYTES:sha256-abc
  second bytes(): BYTES:sha256-abc  <- cached, no resolve
  materialised? yes
```

`mutable` members can be modified even through a `const` object. The justification is in the header's own comment:

> `mutable` cache + const `bytes()`: materialising is not a logical mutation, so it stays available on a const Message.

That's the test for `mutable`: **is this a change to the object's observable state, or just to how it's stored?** A cache is the second. Anything else is almost certainly a design smell.

### `constexpr` vs `const`

```cpp
const int     a = runtime_value();    // const, but not known at compile time
constexpr int b = 7;                  // known WHILE COMPILING
std::array<int, b> arr{};             // needs a compile-time constant
// std::array<int, a> bad{};          // ERROR
```

`const` = "I won't change it". `constexpr` = "the compiler knows its value". Chapter 10 goes deep.

---

## 1.8 Lifetime — where the real bugs are

Everything so far was vocabulary. This is where C++ actually hurts people.

**Rule: an object created in a scope is destroyed when that scope exits, in reverse order of construction.**

```
=== 1.8 lifetime (reverse destruction order) ===
  + a
  + b
  + c
  (leaving inner scope)
  - c
  - b
  (leaving function)
  - a
```

`c` dies before `b`. Deterministic, guaranteed, and the foundation of RAII (Chapter 2).

### Dangling reference: returning a local

```cpp
const std::string& broken() {
    std::string local = "I die at the closing brace";
    return local;          // reference to a dead object
}
```

```
warning: reference to local variable 'local' returned [-Wreturn-local-addr]
```

```sh
$ g++ -fsanitize=address -g dangle.cpp -o a && ./a
ERROR: AddressSanitizer: stack-use-after-return
```

**Fix: return by value.** It's not slow — see §1.9.

### The subtle version the compiler misses

```cpp
struct View {
    const std::string& s;   // storing a reference — hazard
    explicit View(const std::string& str) : s(str) {}
};

std::string make() { return "temporary"; }

View v{make()};   // the temporary dies at the end of THIS LINE
v.print();        // v.s now refers to freed memory
```

No warning. Under ASan: `stack-use-after-scope`.

**Any type that stores a reference or pointer is a lifetime hazard.** That includes `std::string_view` and `std::span`.

### `string_view` — the modern footgun

```cpp
std::string_view bad() {
    std::string s = "hello";
    return s;             // view outlives the string it views
}
```

`string_view` is a pointer + length. It owns nothing. **Perfect as a parameter, lethal as a return type or member** unless you can prove the data outlives it.

agentty uses it correctly in `src/util/logx.cpp`:

```cpp
void emit(Channel ch, Level lv, std::string_view site, std::string_view msg);
```

The caller owns those strings and they outlive the call. That's the safe shape.

### Lifetime extension (real, but narrow)

```cpp
const std::string& r = make();   // temporary's life extended to r's scope
std::printf("%s\n", r.c_str());  // SAFE
```

Binding a temporary to a `const&` **local variable** extends its life. This does *not* apply to function parameters or member references. Rely on it only in this exact shape.

### Iterator invalidation

```
  vector size=3 cap=3
  push -> size=4 cap=6      <- reallocated
  push -> size=5 cap=6
  push -> size=6 cap=6
  push -> size=7 cap=12     <- and again
```

When a vector grows past capacity it allocates a new buffer, moves everything, and frees the old one. **Every pointer, reference and iterator into it dangles.**

```cpp
std::vector<int> v{1, 2, 3};
int& first = v[0];
v.push_back(4);        // may reallocate
std::printf("%d\n", first);   // heap-use-after-free
```

**Rule: don't hold references into a container you're still modifying.**

---

## 1.9 Copy, move, and why returning by value is fast

New programmers avoid returning big objects, fearing a copy. Let's measure.

```
=== 1.9 elision ===
 make_prvalue():
  ctor 1
 make_named():
  ctor 2
 std::move(b):
  move 2
```

- **`make_prvalue()`** — `return Tracked{1};` — *one constructor*. No copy, no move. Since C++17 this is **guaranteed**: the object is built directly in the caller's storage.
- **`make_named()`** — `Tracked t{2}; return t;` — also one constructor. That's NRVO. Not guaranteed by the standard, but every real compiler does it.
- **`std::move(b)`** — one move. The only one that cost anything.

**Returning by value is free. Do it.**

### The pessimisation beginners write

```cpp
T bad()  { T t; return std::move(t); }   // BLOCKS elision — forces a move
T good() { T t; return t; }              // elided — nothing happens
```

`return std::move(t)` makes things **slower**. It turns a name into an xvalue, defeating NRVO. **Never `std::move` a return value of local type.**

### `noexcept` on moves is not optional

```
=== 1.9 noexcept decides copy vs move ===
 vector<Throwing> reallocating:
  COPY
 vector<Safe> reallocating:
  move
```

Same code. One keyword different. One **copies**, the other **moves**.

Why: when `vector` reallocates it must preserve the strong exception guarantee — if anything throws halfway, the vector must be unchanged. A throwing move can't be undone (the source is already gutted), so `vector` refuses to use it. A `noexcept` move can't fail.

This is why agentty's `Id` constructor is marked `noexcept`:

```cpp
explicit Id(std::string s) noexcept : value(std::move(s)) {}
```

`std::vector<ThreadId>` is used throughout the codebase. Without that keyword, every reallocation would deep-copy every string.

**Always mark move constructors and move assignment `noexcept`.**

---

## 1.10 `auto` and `decltype`

### `auto` drops references and const

```cpp
const std::string  s = "hello";
const std::string& r = s;

auto a = s;          // std::string — const and & dropped. A COPY.
auto b = r;          // std::string — still a copy!
const auto& c = s;   // const std::string& — no copy
```

**`auto x = ...` always copies.** Want a reference? Say `auto&` or `const auto&`.

### The loop that silently copies

```
 for (auto e : v):
  COPY
  COPY
 for (const auto& e : v):
 (no output above = no copies)
```

**Default to `for (const auto& x : v)`.** Use `auto&` to modify, plain `auto` only when you genuinely want a copy.

You'll see this everywhere in agentty:

```cpp
for (const auto& m : models_)          // no copies
    if (m.id == id) return &m;
```

### `decltype` and the parentheses trap

```
decltype(x)   is reference? no
decltype((x)) is reference? yes  <- extra parens!
```

One pair of parentheses changes `int` into `int&`. `decltype(x)` gives the *declared type* of the entity; `decltype((x))` gives the type of the *expression*, and an lvalue expression of type `int` yields `int&`.

---

## 1.11 Reading real agentty code

You now know enough to read this properly. From `include/agentty/domain/conversation.hpp`:

```cpp
class ImageContent {
public:
    using Source = LazyBytes::Source;

    std::string media_type;

    ImageContent() = default;
    ImageContent(std::string mt, std::string raw_bytes)
        : media_type(std::move(mt)), data_(std::move(raw_bytes)) {}

    static ImageContent lazy(std::string mt, Source src) {
        ImageContent img;
        img.media_type = std::move(mt);
        img.data_      = LazyBytes::lazy(std::move(src));
        return img;
    }

    [[nodiscard]] const std::string& bytes() const { return data_.bytes(); }
    void set_bytes(std::string raw) { data_.set_bytes(std::move(raw)); }
    [[nodiscard]] bool materialised() const noexcept { return data_.materialised(); }

private:
    LazyBytes data_;
};
```

Work through it:

1. **`ImageContent(std::string mt, std::string raw_bytes)`** — by value, then `std::move` into members. The caller can move in and pay zero copies; if they pass an lvalue they pay exactly one copy, which they were going to pay anyway. (§1.4, §1.9)

2. **`static ImageContent lazy(...)`** — a named constructor. It returns by value, and NRVO means `img` is built directly in the caller's storage — no copy despite appearances. (§1.9)

3. **`const std::string& bytes() const`** — returns a reference to avoid copying potentially megabytes of image data. Safe because the data lives in `data_`, which outlives the call. (§1.6, §1.8)

4. **`[[nodiscard]]`** — calling `bytes()` and ignoring it is pointless work. (§1.3)

5. **`materialised() const noexcept`** — a pure query. `noexcept` because reading a bool cannot throw. (§1.9)

6. **`private: LazyBytes data_;`** — the lazy-loading machinery is hidden. Callers see bytes; they never see the blob store. (Chapter 2)

Every one of those decisions is something you can now justify.

---

## 1.12 Exercises

Reading C++ and writing C++ are different skills.

### 1.1 — Break it on purpose (20 min)

Compile each with `-fsanitize=address,undefined -g`, read the output, then fix it.

```cpp
// (a)
std::vector<int> v{1,2,3};
int& r = v[0];
v.push_back(4);
return r;

// (b)
std::string_view f() { std::string s = "temp"; return s; }

// (c)
int m = std::numeric_limits<int>::max();
return m + 1;
```

For each: what does the sanitizer call it, and *why* does it happen?

### 1.2 — Predict, then verify (30 min)

Using `Tracked` from `ch01_examples.cpp`, predict the output **before** compiling:

```cpp
Tracked a{1};
Tracked b = a;
Tracked c = std::move(a);
std::vector<Tracked> v;
v.push_back(b);
v.push_back(std::move(c));
v.reserve(10);
```

Where were you wrong? (Most people miss the `reserve`.)

### 1.3 — Extend agentty's `Id` (45 min)

Starting from the real `include/agentty/domain/id.hpp`, add:

- `starts_with(std::string_view)` — useful for the `call_salvaged_` prefix check the real codebase does
- a `std::hash` specialisation so `Id` works as an `unordered_map` key
- a `size()` accessor

Then prove `sizeof(Id<T>) == sizeof(std::string)` still holds. Why does adding member *functions* never change the size?

### 1.4 — Find three dangles (30 min)

```cpp
struct Cache {
    std::vector<std::string> items;
    const std::string& first() const { return items.front(); }
    std::string_view longest() const {
        std::string best;
        for (const auto& s : items) if (s.size() > best.size()) best = s;
        return best;
    }
};

int main() {
    Cache c;
    const std::string& f = c.first();
    c.items.push_back("hello");
    auto l = c.longest();
    return (int)(f.size() + l.size());
}
```

Find them with sanitizers. Explain each in one sentence.

### 1.5 — Measure `noexcept` (30 min)

Build a `Buffer` owning a heap allocation. Move constructor **without** `noexcept`, put 10,000 in a vector, time it. Add `noexcept`. Time again. Explain the difference using §1.9.

### 1.6 — Design like agentty (60 min)

Design types for a file tool such that these are **compile errors**:

```cpp
process(output_path, input_path);   // arguments swapped
process("relative/path");           // must be absolute
resize(height, width);              // dimensions swapped
```

Constraint: zero runtime overhead vs raw `std::string`/`int`. Prove it with `sizeof`.

---

## Key takeaways

1. **An object is storage + type + lifetime.** The lifetime part is what makes C++ hard and fast.
2. **If it has a name, it's an lvalue** — including a named `T&&`.
3. **`std::move` moves nothing.** It's a cast; the move happens when something consumes the result.
4. **Prefer `{}`** — it rejects narrowing conversions.
5. **Integer division truncates**, signed overflow is UB, unsigned wraps silently, mixed comparisons lie. Use `-fsanitize=undefined` and `std::cmp_less`.
6. **`const&` for big parameters, by value for small.** ~16 bytes is the line.
7. **Return by value.** It's elided. Never `return std::move(local)`.
8. **Mark moves `noexcept`** or containers silently copy.
9. **`auto` copies.** `const auto&` in range-for by default.
10. **Anything storing a reference or pointer** — including `string_view` — is a lifetime hazard.
11. **Modifying a container invalidates references into it.**
12. **Strong types cost nothing.** Same `sizeof`, same codegen, bugs become compile errors.

---

## Flags to use from now on

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic \
    -fsanitize=address,undefined -g \
    yourfile.cpp -o yourfile
```

Turn them off for release builds. Never for learning.

---

## agentty files you can now read

- `include/agentty/domain/id.hpp` — the whole thing
- `include/agentty/domain/lazy_bytes.hpp` — `mutable`, const-correctness, ownership
- `include/agentty/domain/conversation.hpp` lines 41–72 — `ImageContent`

Open them. You'll recognise every construct.

---

## Next chapter

**Chapter 2: Memory Management — RAII and Ownership** takes §1.8's lifetime rules and turns them into a design discipline: how destructors run automatically, why that makes C++ resource handling safer than `try/finally`, and how ownership becomes visible in a type.
