[← exercises](exercises.md) · [chapter index](README.md)

# Chapter 1 quick reference

One page. For after you've read the sections, not instead.

---

## parameter passing

| you want to | write | cost |
|---|---|---|
| read it | `const T&` | free |
| read a small trivial type | `T` | a register |
| read a string you won't keep | `std::string_view` | free |
| modify the caller's object | `T&` | free |
| keep a copy | `T`, then `std::move` into place | one copy or move |
| take ownership of an rvalue only | `T&&` | free |

---

## reference binding

| | lvalue | rvalue |
|---|---|---|
| `T&` | yes | no |
| `const T&` | yes | yes |
| `T&&` | no | yes |
| `auto&&` | yes | yes |

---

## value categories

| | identity | movable |
|---|---|---|
| lvalue (`x`, `*p`, `arr[0]`) | yes | no |
| xvalue (`std::move(x)`) | yes | yes |
| prvalue (`42`, `f()` returning `T`) | no | yes |

A **named** `T&&` parameter is an **lvalue**. Re-`move` it to forward it.

---

## initialisation

```cpp
T x{};              // value-init. the safe default.
T x{a, b};          // list-init. rejects narrowing.
T x(count, value);  // when a ctor takes a count
auto x = expr;      // when the type is obvious from the right
T x{.a = 1, .b = 2};// aggregates. must be in declaration order.
```

Gotchas:
- `std::vector<int> v{3, 7}` is two elements, `v(3, 7)` is three sevens
- `std::string s();` declares a function
- `int x;` in a function leaves garbage. `int x{};` is 0.

---

## integers

```cpp
i + 1 < v.size()               // never  i < v.size() - 1
std::cmp_less(s, u)            // never  s < u  across signedness
x > INT_MAX - 1                // check BEFORE adding, not after
std::uint8_t b{300}            // compile error. `= 300` is silently 44.
```

Signed overflow is UB. Unsigned overflow wraps and is defined.

---

## the special six

```cpp
T();  ~T();
T(const T&);  T& operator=(const T&);
T(T&&) noexcept;  T& operator=(T&&) noexcept;
```

**Rule of zero:** declare none. Let members manage themselves.
**Rule of five:** declare one, declare all five.
Declaring a destructor **suppresses the implicit moves**.

Every move gets `noexcept`, or `vector` copies instead of moving.

---

## returns

```cpp
T f() { return T{...}; }      // RVO. guaranteed since C++17. free.
T f() { T x; return x; }      // NRVO. allowed, every compiler does it.
T f() { return std::move(x); }// NEVER. blocks elision, adds a move.
```

Return by value. It costs nothing.

---

## const

- `const T&` = *I* won't write through this. Others still might.
- `const T` object = nobody writes, through any path.
- `const` member function = can be called on a const object. Mark
  everything you can.
- `mutable` = writable through const. Only when two const calls are
  indistinguishable, and never across threads without a mutex.

---

## auto

```cpp
auto  x = expr;        // drops & and top-level const
auto& x = expr;        // keeps the reference
const auto& x = expr;  // read-only view, no copy
auto&& x = expr;       // binds to anything
decltype(auto) x = f();// exact type, references kept
```

Range-for:
```cpp
for (const auto& e : v)   // default
for (auto& e : v)         // modifying
for (auto e : v)          // deliberate copy, or cheap types
```

`decltype(x)` is the declared type. `decltype((x))` is `T&`.

---

## agentty patterns

**Strong id** (`domain/id.hpp`):
```cpp
template <typename Tag> struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};
```

**Sink parameter:**
```cpp
explicit T(std::string s) : member_(std::move(s)) {}
```

**Lazy const cache** (`domain/lazy_bytes.hpp`):
```cpp
mutable std::string cache_;
mutable bool        resolved_ = false;
const std::string& get() const {
    if (!resolved_) { cache_ = resolve(); resolved_ = true; }
    return cache_;
}
bool empty() const noexcept {           // does NOT call get()
    return resolved_ ? cache_.empty() : source_.empty();
}
```

**Hidden friend for ADL:**
```cpp
friend void to_json(nlohmann::json& j, const T& t) { j = t.value; }
```

---

## build flags

```sh
g++ -std=c++23 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined
```

ASan catches use-after-free, stack-use-after-scope, heap overflow, leaks.
UBSan catches signed overflow, bad shifts, misaligned access, bad enum
values. Both cost about 2x. Worth it on every debug build.

---

## the warnings that are actually bugs

| warning | means |
|---|---|
| `-Wsign-compare` | signed/unsigned comparison. wrong answer. |
| `-Wreturn-local-addr` | returning a reference to a dead local. |
| `-Wdangling-pointer` | saved a pointer to something that died. |
| `-Wpessimizing-move` | `return std::move(x)` blocking elision. |
| `-Wreorder` | init list order differs from declaration order. |
| `-Wunused-result` | discarded a `[[nodiscard]]`. |

None of these are style. All of them are bugs.

---

[chapter index](README.md) · [chapter 2: RAII and ownership →](../ch02-memory/)
