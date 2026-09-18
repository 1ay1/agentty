# Chapter 12: Template Metaprogramming

**Learning Objectives:**
- Understand compile-time type computation
- Master SFINAE (Substitution Failure Is Not An Error)
- Use tag dispatch for algorithm selection
- Write type traits and metafunctions
- See how agentty's Id<Tag> system works under the hood
- Build compile-time decision trees

---

## 12.1 What Is Template Metaprogramming?

### Computing Types, Not Values

Regular programming: compute **values** at runtime.

```cpp
int add(int a, int b) { return a + b; }
```

Template metaprogramming: compute **types** at **compile time**.

```cpp
template<typename T>
struct AddPointer {
    using type = T*;
};

AddPointer<int>::type x;  // x is int*
```

**The program:** Type transformations expressed as templates.  
**The execution:** Happens during compilation.  
**The output:** Types, not values.

### Why It Matters

1. **Zero runtime cost** — all computation done at compile time
2. **Type safety** — errors caught before runtime
3. **Generic programming** — write once, work for any type
4. **Library infrastructure** — powers std::vector, std::unique_ptr, etc.

---

## 12.2 Type Traits

### Basic Type Traits

The `<type_traits>` header provides compile-time type information:

```cpp
#include <type_traits>

static_assert(std::is_integral_v<int>);           // true
static_assert(std::is_floating_point_v<double>);  // true
static_assert(std::is_pointer_v<int*>);           // true
static_assert(std::is_const_v<const int>);        // true
```

### Removing Qualifiers

```cpp
// Remove const
std::remove_const_t<const int>  // int

// Remove reference
std::remove_reference_t<int&>   // int
std::remove_reference_t<int&&>  // int

// Remove pointer
std::remove_pointer_t<int*>     // int

// Chain them
std::remove_const_t<std::remove_reference_t<const int&>>  // int
```

### Adding Qualifiers

```cpp
std::add_pointer_t<int>       // int*
std::add_const_t<int>         // const int
std::add_lvalue_reference_t<int>  // int&
std::add_rvalue_reference_t<int>  // int&&
```

---

## 12.3 SFINAE: Substitution Failure Is Not An Error

### The Principle

When the compiler tries to instantiate a template and substitution fails, it's **not an error** — the template is just removed from the candidate set.

**Example:**

```cpp
// Overload 1: only for integral types
template<typename T>
typename std::enable_if_t<std::is_integral_v<T>, void>
process(T value) {
    std::cout << "Integer: " << value << '\n';
}

// Overload 2: only for floating point
template<typename T>
typename std::enable_if_t<std::is_floating_point_v<T>, void>
process(T value) {
    std::cout << "Float: " << value << '\n';
}

process(42);    // Calls overload 1
process(3.14);  // Calls overload 2
process("hi");  // ERROR: no viable overload
```

**What happens:**
- `process(42)`: Overload 2's substitution fails (int not floating point), so it's removed. Overload 1 succeeds.
- `process(3.14)`: Overload 1's substitution fails (double not integral). Overload 2 succeeds.
- `process("hi")`: Both fail, so it's a real error.

### std::enable_if

The workhorse of SFINAE:

```cpp
template<bool Condition, typename T = void>
struct enable_if {};

template<typename T>
struct enable_if<true, T> {
    using type = T;
};
```

**Usage:**
- If `Condition` is true, `enable_if_t<Condition>` is `void`
- If `Condition` is false, `enable_if_t<Condition>` doesn't exist (SFINAE)

---

## 12.4 Real Example: Type-Safe Id<Tag>

### The Problem

agentty has many ID types:

```cpp
ThreadId thread_id = "thread-1234";
MessageId msg_id = "msg-5678";
```

We want:
1. Strong typing (can't mix ThreadId and MessageId)
2. Zero runtime overhead
3. Generic implementation (one template for all IDs)

### The Solution: Id<Tag> with SFINAE

`agentty/include/agentty/core/ids.hpp`:

```cpp
// Tags define the ID type
struct ThreadTag {
    static constexpr std::string_view name = "thread";
};

struct MessageTag {
    static constexpr std::string_view name = "message";
};

// Generic Id wrapper
template<typename Tag>
class Id {
    std::string value_;
    
public:
    explicit Id(std::string v) : value_(std::move(v)) {}
    
    std::string_view get() const { return value_; }
    
    // Only compare with same tag
    bool operator==(const Id<Tag>& other) const {
        return value_ == other.value_;
    }
    
    // Can't compare different tags (won't compile)
    template<typename OtherTag>
    bool operator==(const Id<OtherTag>&) const = delete;
};

using ThreadId = Id<ThreadTag>;
using MessageId = Id<MessageTag>;
```

**Usage:**

```cpp
ThreadId t1{"thread-1"};
ThreadId t2{"thread-2"};
MessageId m1{"msg-1"};

t1 == t2;  // ✅ OK
t1 == m1;  // ❌ ERROR: use of deleted function
```

### Adding Hash Support with SFINAE

We want `std::hash<Id<Tag>>` to work **only if** Tag has a `name` member:

```cpp
namespace std {
    template<typename Tag>
    struct hash<Id<Tag>> {
        // SFINAE: only enabled if Tag::name exists
        template<typename T = Tag>
        auto operator()(const Id<T>& id) const
            -> std::enable_if_t<
                std::is_convertible_v<decltype(T::name), std::string_view>,
                size_t
            >
        {
            return std::hash<std::string_view>{}(id.get());
        }
    };
}
```

**Result:** `std::unordered_map<ThreadId, ...>` works automatically.

---

## 12.5 Tag Dispatch

### The Pattern

Use **empty types** (tags) to select overloads at compile time:

```cpp
// Tags
struct FastTag {};
struct SlowTag {};

// Overloads
void algorithm_impl(int n, FastTag) {
    // Fast implementation
}

void algorithm_impl(int n, SlowTag) {
    // Slow but general implementation
}

// Dispatch based on size
template<typename T>
void algorithm(T value) {
    if constexpr (sizeof(T) <= 8) {
        algorithm_impl(value, FastTag{});
    } else {
        algorithm_impl(value, SlowTag{});
    }
}
```

**Benefits:**
- Clear separation of algorithm variants
- Zero runtime cost (tag types have no data)
- Easy to extend (add more tags)

### Real Example: Iterator Categories

The standard library uses tag dispatch for iterators:

```cpp
struct input_iterator_tag {};
struct forward_iterator_tag : public input_iterator_tag {};
struct bidirectional_iterator_tag : public forward_iterator_tag {};
struct random_access_iterator_tag : public bidirectional_iterator_tag {};

// std::advance implementation
template<typename It>
void advance_impl(It& it, int n, std::random_access_iterator_tag) {
    it += n;  // O(1)
}

template<typename It>
void advance_impl(It& it, int n, std::input_iterator_tag) {
    while (n--) ++it;  // O(n)
}

template<typename It>
void advance(It& it, int n) {
    advance_impl(it, n, typename std::iterator_traits<It>::iterator_category{});
}
```

**Effect:**
- `std::advance(vec.begin(), 10)` — O(1) (random access)
- `std::advance(list.begin(), 10)` — O(n) (bidirectional)

---

## 12.6 Compile-Time Recursion

### Factorial at Compile Time (Old Style)

Before `constexpr`, metaprogramming used recursive templates:

```cpp
template<int N>
struct Factorial {
    static constexpr int value = N * Factorial<N - 1>::value;
};

template<>
struct Factorial<0> {
    static constexpr int value = 1;
};

constexpr int x = Factorial<5>::value;  // 120
```

**How it works:**
- `Factorial<5>` → `5 * Factorial<4>`
- `Factorial<4>` → `4 * Factorial<3>`
- ...
- `Factorial<0>` → `1`

### Modern: Use constexpr Functions

Today, prefer `constexpr` (simpler, clearer):

```cpp
constexpr int factorial(int n) {
    return (n == 0) ? 1 : n * factorial(n - 1);
}

constexpr int x = factorial(5);
```

**When to use recursive templates:**
- Type transformations (can't do with constexpr)
- Building compile-time data structures

---

## 12.7 Type Lists

### Building Compile-Time Lists of Types

```cpp
template<typename... Ts>
struct TypeList {};

// Check if type is in list
template<typename T, typename List>
struct Contains;

template<typename T, typename... Ts>
struct Contains<T, TypeList<T, Ts...>> : std::true_type {};

template<typename T, typename U, typename... Ts>
struct Contains<T, TypeList<U, Ts...>> : Contains<T, TypeList<Ts...>> {};

template<typename T>
struct Contains<T, TypeList<>> : std::false_type {};

// Usage
using AllowedTypes = TypeList<int, double, std::string>;
static_assert(Contains<int, AllowedTypes>::value);
static_assert(!Contains<float, AllowedTypes>::value);
```

---

## 12.8 std::conditional: Compile-Time if

Choose a type based on a condition:

```cpp
template<bool Condition, typename TrueType, typename FalseType>
struct conditional {
    using type = TrueType;
};

template<typename TrueType, typename FalseType>
struct conditional<false, TrueType, FalseType> {
    using type = FalseType;
};

// Helper
template<bool Condition, typename TrueType, typename FalseType>
using conditional_t = typename conditional<Condition, TrueType, FalseType>::type;
```

**Example:**

```cpp
template<typename T>
using StorageType = std::conditional_t<
    (sizeof(T) <= 8),
    T,          // Store by value if small
    T*          // Store by pointer if large
>;

StorageType<int> small;       // int
StorageType<std::string> big; // std::string*
```

---

## 12.9 Real Example: agentty's Variant Type Selection

### The Problem

agentty's `Msg` is a variant of 60+ message types. Some are small (8 bytes), some are large (200+ bytes). We want:

- Small types stored **inline**
- Large types stored **on the heap**

### The Solution: Conditional Storage

```cpp
template<typename T>
struct MsgStorage {
    using type = std::conditional_t<
        (sizeof(T) <= 64),  // Threshold
        T,                  // Inline
        std::unique_ptr<T>  // Heap
    >;
};

// Usage in variant
using Msg = std::variant<
    MsgStorage<ComposerEnter>::type,
    MsgStorage<StreamTextDelta>::type,
    MsgStorage<ToolCallComplete>::type,
    // ...
>;
```

**Result:**
- Small messages (e.g., `ComposerEnter`) — no allocation
- Large messages (e.g., `ToolCallComplete` with 2KB payload) — heap-allocated

---

## 12.10 Detecting Members with SFINAE

### Check if a Type Has a Method

```cpp
// Detect if T has a .size() method
template<typename T, typename = void>
struct HasSize : std::false_type {};

template<typename T>
struct HasSize<T, std::void_t<decltype(std::declval<T>().size())>>
    : std::true_type {};

// Usage
static_assert(HasSize<std::vector<int>>::value);
static_assert(!HasSize<int>::value);
```

**How it works:**
- `std::declval<T>()` — pretend you have a T
- `decltype(...)` — get the type of the expression
- `std::void_t` — if the expression is valid, this succeeds
- If expression invalid, SFINAE removes this specialization

---

## 12.11 Real Example: Serialization Dispatch

agentty serializes values to JSON. Different types need different handling:

```cpp
// Default: use .to_json() if available
template<typename T>
auto serialize(const T& value)
    -> decltype(value.to_json())  // SFINAE: only if to_json() exists
{
    return value.to_json();
}

// Fallback: arithmetic types
template<typename T>
std::enable_if_t<std::is_arithmetic_v<T>, nlohmann::json>
serialize(const T& value) {
    return value;
}

// Fallback: strings
nlohmann::json serialize(const std::string& value) {
    return value;
}
```

**Result:**
- Custom types with `to_json()` — calls that
- Numbers — direct conversion
- Strings — direct conversion
- Others — compile error

---

## 12.12 Variadic Template Tricks

### Count Arguments

```cpp
template<typename... Ts>
constexpr size_t count_args() {
    return sizeof...(Ts);
}

static_assert(count_args<int, double, char>() == 3);
```

### Get Nth Type

```cpp
template<size_t N, typename T, typename... Ts>
struct NthType {
    using type = typename NthType<N - 1, Ts...>::type;
};

template<typename T, typename... Ts>
struct NthType<0, T, Ts...> {
    using type = T;
};

// Helper
template<size_t N, typename... Ts>
using NthType_t = typename NthType<N, Ts...>::type;

// Usage
using Second = NthType_t<1, int, double, char>;  // double
```

### Check if All Types Satisfy Predicate

```cpp
template<template<typename> class Pred, typename... Ts>
constexpr bool all_satisfy = (Pred<Ts>::value && ...);

// Usage
static_assert(all_satisfy<std::is_integral, int, long, short>);
static_assert(!all_satisfy<std::is_integral, int, double>);
```

---

## 12.13 Perfect Forwarding Revisited

### The Problem

You want a wrapper that forwards arguments without changing their value category:

```cpp
template<typename Func, typename... Args>
void call_twice(Func&& func, Args&&... args) {
    func(args...);  // BAD: args become lvalues
    func(args...);
}
```

### The Solution: std::forward

```cpp
template<typename Func, typename... Args>
void call_twice(Func&& func, Args&&... args) {
    func(std::forward<Args>(args)...);
    func(std::forward<Args>(args)...);
}
```

**How std::forward works:**

```cpp
template<typename T>
constexpr T&& forward(std::remove_reference_t<T>& t) noexcept {
    return static_cast<T&&>(t);
}
```

**Magic:**
- If `T` is `int&`, returns `int&` (lvalue)
- If `T` is `int&&`, returns `int&&` (rvalue)

---

## 12.14 Real Example: maya's Cmd Builder

maya uses variadic templates to build effect commands:

```cpp
// Builder
template<typename Msg, typename... Cmds>
auto batch(Cmds&&... cmds) {
    return BatchCmd<Msg>{std::forward<Cmds>(cmds)...};
}

// Usage
auto cmds = batch<Msg>(
    task(compute_something),
    http_request(url),
    delay(100ms)
);
```

**Why it works:**
- Variadic template accepts any number of commands
- Perfect forwarding preserves temporary/reference status
- Zero overhead (all inline)

---

## 12.15 Compile-Time Strings (C++20)

### The Problem

You want to pass a string as a template parameter:

```cpp
template<const char* Str>  // Can't use string literal directly
struct Message {};
```

### The Solution: Class Types as Template Parameters

C++20 allows class types with `operator<=>`:

```cpp
template<size_t N>
struct FixedString {
    char data[N];
    
    constexpr FixedString(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }
    
    auto operator<=>(const FixedString&) const = default;
};

template<FixedString Str>
struct Message {
    static constexpr auto value = Str.data;
};

// Usage
Message<"hello"> msg;
std::cout << msg.value;  // "hello"
```

---

## 12.16 When NOT to Use Metaprogramming

### Readability

Complex metaprogramming can obscure intent:

```cpp
// BAD: cryptic
template<typename T>
using Foo = std::conditional_t<
    std::is_same_v<std::remove_const_t<T>, int>,
    double,
    T
>;

// GOOD: clear
template<typename T>
using Foo = std::conditional_t<
    std::is_same_v<T, int> || std::is_same_v<T, const int>,
    double,
    T
>;
```

### Compile Time

Heavy metaprogramming slows compilation:

```cpp
// BAD: 10 levels of recursion
template<int N>
struct Fib {
    static constexpr int value = Fib<N-1>::value + Fib<N-2>::value;
};

// GOOD: constexpr function (much faster to compile)
constexpr int fib(int n) {
    return (n <= 1) ? n : fib(n-1) + fib(n-2);
}
```

**Rule:** Use `constexpr` for values, metaprogramming for types.

---

## 12.17 Summary

**What we learned:**
- ✅ Template metaprogramming computes **types** at compile time
- ✅ SFINAE removes invalid overloads without errors
- ✅ `std::enable_if` enables conditional overloading
- ✅ Tag dispatch selects algorithms based on types
- ✅ Type traits query and transform types
- ✅ Variadic templates handle arbitrary arguments
- ✅ Perfect forwarding preserves value categories

**Key insight:** Metaprogramming is the **language inside the language** — types are data, templates are functions.

---

## 12.18 Exercises

### Exercise 1: Implement remove_pointer

Write your own `remove_pointer` metafunction:

```cpp
template<typename T>
struct RemovePointer {
    using type = /* YOUR CODE */;
};

// Test
static_assert(std::is_same_v<RemovePointer<int*>::type, int>);
static_assert(std::is_same_v<RemovePointer<int>::type, int>);
```

<details>
<summary>Solution</summary>

```cpp
template<typename T>
struct RemovePointer {
    using type = T;
};

template<typename T>
struct RemovePointer<T*> {
    using type = T;
};

template<typename T>
using RemovePointer_t = typename RemovePointer<T>::type;
```

</details>

---

### Exercise 2: Detect if Type Has to_string()

Write a type trait `HasToString<T>` that checks if `T` has a `to_string()` method:

```cpp
template<typename T, typename = void>
struct HasToString : std::false_type {};

template<typename T>
struct HasToString<T, /* YOUR CODE */> : std::true_type {};

struct Foo { std::string to_string() const; };
struct Bar {};

static_assert(HasToString<Foo>::value);
static_assert(!HasToString<Bar>::value);
```

<details>
<summary>Solution</summary>

```cpp
template<typename T, typename = void>
struct HasToString : std::false_type {};

template<typename T>
struct HasToString<T, std::void_t<decltype(std::declval<T>().to_string())>>
    : std::true_type {};
```

</details>

---

### Exercise 3: Conditional Return Type

Write a function `get_value()` that returns:
- `int` if `T` is integral
- `double` if `T` is floating point
- `std::string` otherwise

```cpp
template<typename T>
/* YOUR RETURN TYPE */ get_value(T value) {
    // YOUR CODE
}
```

<details>
<summary>Solution</summary>

```cpp
template<typename T>
using ReturnType = std::conditional_t<
    std::is_integral_v<T>, int,
    std::conditional_t<
        std::is_floating_point_v<T>, double,
        std::string
    >
>;

template<typename T>
ReturnType<T> get_value(T value) {
    if constexpr (std::is_integral_v<T>) {
        return static_cast<int>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<double>(value);
    } else {
        return std::to_string(value);
    }
}
```

</details>

---

### Exercise 4: Type List Search

Implement `IndexOf<T, TypeList<Ts...>>` that returns the index of `T` in the type list:

```cpp
template<typename T, typename List>
struct IndexOf;

// YOUR CODE

using MyList = TypeList<int, double, char>;
static_assert(IndexOf<double, MyList>::value == 1);
```

<details>
<summary>Solution</summary>

```cpp
template<typename T, typename List>
struct IndexOf;

template<typename T, typename... Ts>
struct IndexOf<T, TypeList<T, Ts...>> {
    static constexpr size_t value = 0;
};

template<typename T, typename U, typename... Ts>
struct IndexOf<T, TypeList<U, Ts...>> {
    static constexpr size_t value = 1 + IndexOf<T, TypeList<Ts...>>::value;
};
```

</details>

---

### Exercise 5: SFINAE Overload Selection

Write two overloads of `print()`:
1. For types with `to_string()` — call it
2. For arithmetic types — use `std::to_string()`

```cpp
struct Foo {
    std::string to_string() const { return "Foo"; }
};

print(Foo{});  // "Foo"
print(42);     // "42"
```

<details>
<summary>Solution</summary>

```cpp
// Overload 1: types with to_string()
template<typename T>
auto print(const T& value)
    -> decltype(value.to_string(), void())
{
    std::cout << value.to_string() << '\n';
}

// Overload 2: arithmetic types
template<typename T>
std::enable_if_t<std::is_arithmetic_v<T>, void>
print(T value) {
    std::cout << std::to_string(value) << '\n';
}
```

</details>

---

## Next Chapter

In [Chapter 13: Type-Level Proofs with consteval](../ch13-proofs/README.md), we'll use compile-time computation to **prove** invariants, not just check them.

**Preview:** How agentty guarantees its tool catalog is complete without any runtime checks.

---

## Further Reading

- [SFINAE (cppreference)](https://en.cppreference.com/w/cpp/language/sfinae)
- [Type Traits (cppreference)](https://en.cppreference.com/w/cpp/header/type_traits)
- agentty source: `include/agentty/core/ids.hpp`
- agentty source: `include/agentty/util/hash.hpp`

---

**Previous:** [Chapter 10: constexpr and Compile-Time Computation](../../part2-modern-patterns/ch10-constexpr/README.md)  
**Next:** [Chapter 13: Type-Level Proofs](../ch13-proofs/README.md)  
**Up:** [Part III: Advanced Techniques](../README.md)
