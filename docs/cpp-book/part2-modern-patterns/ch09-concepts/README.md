# Chapter 9: Concepts and Constraints

**Learning Objectives:**
- Understand what concepts are and why they matter
- Define custom concepts for template constraints
- Use standard library concepts effectively
- See how agentty uses concepts for type safety
- Write clear, constrained template APIs

---

## 9.1 What Are Concepts?

### The Problem: Inscrutable Template Errors

Before C++20, template errors were legendary for their unreadability:

```cpp
// Pre-C++20: unconstrained template
template<typename T>
auto do_something(T value) {
    return value + 1;  // What if T doesn't support +?
}

do_something(std::string("hello"));  
// ERROR: 150 lines of template instantiation backtrace
```

The error happens deep inside the template instantiation, producing pages of compiler noise.

### The Solution: Concepts

**Concepts** are named constraints on template parameters. They let you specify requirements **at the interface**, not deep in the body:

```cpp
// C++20: constrained with concept
template<typename T>
requires std::integral<T>
auto do_something(T value) {
    return value + 1;
}

do_something(std::string("hello"));
// ERROR: constraints not satisfied
//   std::integral<std::string> evaluated to false
```

Clean, immediate, actionable.

### Why Concepts Matter

1. **Clear errors** — failures at the interface, not in the depths
2. **Self-documenting APIs** — the signature tells you what's required
3. **Overload resolution** — pick the best match based on constraints
4. **Compile-time guarantees** — wrong types can't even be instantiated

---

## 9.2 Defining Concepts

### Basic Syntax

A concept is a compile-time boolean:

```cpp
template<typename T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;
};
```

**Breakdown:**
- `concept Addable` — defines a named concept
- `requires(T a, T b)` — introduces requirement block with variables
- `{ a + b }` — expression must be valid
- `-> std::convertible_to<T>` — result must convert to T

### Using the Concept

```cpp
template<Addable T>
T add(T a, T b) {
    return a + b;
}

add(3, 4);           // ✅ int is Addable
add(3.5, 2.1);       // ✅ double is Addable
add("hello", "hi");  // ❌ const char* not Addable
```

### Multiple Requirements

Concepts can check multiple conditions:

```cpp
template<typename T>
concept Numeric = requires(T a, T b) {
    { a + b } -> std::same_as<T>;
    { a - b } -> std::same_as<T>;
    { a * b } -> std::same_as<T>;
    { a / b } -> std::same_as<T>;
    requires std::is_arithmetic_v<T>;  // nested requirement
};
```

---

## 9.3 Standard Library Concepts

C++20 provides dozens of standard concepts in `<concepts>` and `<iterator>`:

### Type Categories

```cpp
#include <concepts>

// Basic type properties
std::integral<int>           // true
std::floating_point<double>  // true
std::signed_integral<size_t> // false (unsigned)

// Relationships
std::same_as<int, int32_t>         // true
std::convertible_to<int, double>   // true
std::derived_from<Dog, Animal>     // true if Dog : Animal
```

### Comparison Concepts

```cpp
std::equality_comparable<std::string>  // has == and !=
std::totally_ordered<int>              // has <, >, <=, >=, ==, !=
```

### Callable Concepts

```cpp
template<typename F, typename... Args>
concept Invocable = requires(F f, Args... args) {
    std::invoke(f, args...);  // can call f(args...)
};
```

### Iterator Concepts

```cpp
std::input_iterator<std::istream_iterator<int>>
std::forward_iterator<std::list<int>::iterator>
std::random_access_iterator<std::vector<int>::iterator>
```

---

## 9.4 Real Example: agentty's Constrained Types

### Case Study: Id<Tag> with Concepts

Recall `agentty/include/agentty/core/ids.hpp`:

```cpp
// The Id<Tag> newtype requires Tag to have a name
template<typename Tag>
concept IdTag = requires {
    { Tag::name } -> std::convertible_to<std::string_view>;
};

template<IdTag Tag>
class Id {
    std::string value_;
public:
    explicit Id(std::string v) : value_(std::move(v)) {}
    
    std::string_view get() const { return value_; }
    
    // Equality only for same tag
    bool operator==(const Id<Tag>& other) const {
        return value_ == other.value_;
    }
};
```

**Without the concept:**
- You could write `Id<int>` — nonsensical
- Error would happen when trying to access `int::name`
- Deep in template instantiation

**With the concept:**
```cpp
Id<int> bad;  
// ERROR: constraints not satisfied
//   IdTag<int> is false
//   'int' has no member 'name'
```

Clear, immediate, at the definition point.

### Defining Tags with the Concept

```cpp
struct ThreadTag {
    static constexpr std::string_view name = "thread";
};

struct MessageTag {
    static constexpr std::string_view name = "message";
};

using ThreadId = Id<ThreadTag>;    // ✅ ThreadTag satisfies IdTag
using MessageId = Id<MessageTag>;  // ✅ MessageTag satisfies IdTag
```

---

## 9.5 Using Concepts in Templates

### Four Syntaxes

C++20 offers multiple ways to constrain templates:

#### 1. Requires Clause (Trailing)

```cpp
template<typename T>
requires std::integral<T>
T increment(T value) {
    return value + 1;
}
```

#### 2. Concept as Type (Terse Syntax)

```cpp
template<std::integral T>
T increment(T value) {
    return value + 1;
}
```

#### 3. Abbreviated Function Template

```cpp
auto increment(std::integral auto value) {
    return value + 1;
}
```

#### 4. Requires Expression (Inline)

```cpp
template<typename T>
T increment(T value) requires std::integral<T> {
    return value + 1;
}
```

**Recommendation:** Use **syntax 2** (concept as type) for clarity.

### Combining Concepts

**Conjunction (AND):**
```cpp
template<typename T>
concept SignedIntegral = std::integral<T> && std::signed_integral<T>;
```

**Disjunction (OR):**
```cpp
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;
```

**Negation:**
```cpp
template<typename T>
concept NotPointer = !std::is_pointer_v<T>;
```

---

## 9.6 Real Example: Provider Constraints in agentty

### The Problem

agentty supports multiple AI providers (Anthropic, OpenAI, Ollama). Each has a `stream()` function, but with slight variations. We want to ensure **all providers** implement the streaming interface.

### Without Concepts

```cpp
// Just duck typing — error happens when you use it wrong
template<typename P>
StreamResult stream_request(P& provider, const Request& req) {
    return provider.stream(req, /* sink */);
}
```

### With Concepts

`agentty/include/agentty/provider/concept.hpp`:

```cpp
template<typename P>
concept Provider = requires(P p, const Request& req, EventSink sink) {
    // Must have stream() returning StreamResult
    { p.stream(req, sink) } -> std::same_as<provider::StreamResult>;
    
    // Must have name()
    { p.name() } -> std::convertible_to<std::string_view>;
    
    // Must have capabilities()
    { p.capabilities() } -> std::same_as<ProviderCapabilities>;
};
```

**Now the streaming layer can require it:**

```cpp
template<Provider P>
StreamResult stream_request(P& provider, const Request& req) {
    // Guaranteed: P has stream(), name(), capabilities()
    return provider.stream(req, /* sink */);
}
```

**Attempt to pass a non-provider:**
```cpp
stream_request(42, req);
// ERROR: Provider<int> is false
//   'int' has no member 'stream'
```

### Benefits

1. **Compile-time checking** — can't even instantiate with wrong type
2. **Clear requirements** — new provider author sees exactly what to implement
3. **Self-documenting** — the concept IS the documentation

---

## 9.7 Concepts for Overload Resolution

Concepts enable **constraint-based overloading** — picking the most specific match:

```cpp
template<typename T>
void process(T value) {
    std::cout << "Generic\n";
}

template<std::integral T>
void process(T value) {
    std::cout << "Integer\n";
}

template<std::floating_point T>
void process(T value) {
    std::cout << "Float\n";
}

process(42);      // "Integer" (most constrained match)
process(3.14);    // "Float"
process("hello"); // "Generic"
```

**The rule:** The most constrained overload wins.

---

## 9.8 Nested Requirements and Type Aliases

### Nested Requirements

Check **multiple** conditions within a concept:

```cpp
template<typename T>
concept Container = requires(T c) {
    typename T::value_type;           // must have value_type
    { c.size() } -> std::convertible_to<size_t>;
    { c.begin() } -> std::input_iterator;
    { c.end() } -> std::input_iterator;
    requires std::same_as<
        decltype(c.begin()), 
        decltype(c.end())
    >;
};
```

### Compound Requirements

Use `requires requires` for inline checks:

```cpp
template<typename T>
void print(T value) requires requires { std::cout << value; } {
    std::cout << value << '\n';
}
```

The first `requires` introduces the constraint clause, the second starts the requirement block.

---

## 9.9 Real Example: Thread Safety Constraints

### The Problem

agentty's `RankedMutex` should only lock types that are actually mutexes. Without concepts, you could pass anything:

```cpp
RankedMutex<int, 0> bad;  // Nonsense — int is not a mutex
```

### Solution: Mutex Concept

`agentty/include/agentty/concurrency/ranked_mutex.hpp`:

```cpp
template<typename M>
concept BasicLockable = requires(M m) {
    m.lock();
    m.unlock();
};

template<typename M>
concept Lockable = BasicLockable<M> && requires(M m) {
    { m.try_lock() } -> std::same_as<bool>;
};

template<Lockable M, size_t Rank>
class RankedMutex {
    M mutex_;
    // ... rank tracking ...
public:
    void lock() {
        check_rank_order(Rank);
        mutex_.lock();
    }
    
    void unlock() {
        mutex_.unlock();
        release_rank(Rank);
    }
};
```

**Now:**
```cpp
RankedMutex<std::mutex, 0> good;      // ✅
RankedMutex<int, 0> bad;              // ❌ Lockable<int> is false
```

---

## 9.10 Performance: Concepts Are Zero-Cost

Concepts are a **compile-time** feature:

- No runtime overhead
- No code generation
- Pure constraint checking

**Proof:** Check the assembly:

```cpp
template<std::integral T>
T add(T a, T b) { return a + b; }

add(3, 4);
```

**Generated assembly (with -O2):**
```asm
add(int, int):
    lea eax, [rdi + rsi]  ; just a + b
    ret
```

No trace of the concept — it's only enforced at compile time.

---

## 9.11 Debugging Concept Failures

### Reading Concept Errors

When a concept fails, modern compilers show **why**:

```cpp
template<std::integral T>
T increment(T value) { return value + 1; }

increment(3.14);
```

**GCC/Clang output:**
```
error: use of function 'T increment(T) [with T = double]' 
       with unsatisfied constraints
note: the required constraints were not satisfied
note: concept 'std::integral<T> [with T = double]' was not satisfied
```

Clear: `double` doesn't satisfy `std::integral`.

### Debugging Custom Concepts

For your own concepts, **name the requirement**:

```cpp
template<typename T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;  // if this fails, error points here
};
```

The error will show exactly which requirement failed.

---

## 9.12 Common Patterns

### Pattern 1: Type Trait Wrapper

Wrap a type trait in a concept for cleaner syntax:

```cpp
template<typename T>
concept Trivial = std::is_trivially_copyable_v<T>;
```

### Pattern 2: Subsumption (Refinement)

One concept can **subsume** another:

```cpp
template<typename T>
concept Integral = std::is_integral_v<T>;

template<typename T>
concept SignedIntegral = Integral<T> && std::is_signed_v<T>;
```

`SignedIntegral` is **more constrained** than `Integral`, so it wins in overload resolution.

### Pattern 3: Requires Expression in Concept

Check complex conditions:

```cpp
template<typename T>
concept Hashable = requires(T a) {
    { std::hash<T>{}(a) } -> std::convertible_to<size_t>;
};
```

---

## 9.13 When NOT to Use Concepts

### Don't Over-Constrain

If a function works with **any** type, don't add artificial constraints:

```cpp
// BAD: unnecessary constraint
template<std::copyable T>
void log(const T& value) {
    std::cout << value << '\n';
}

// GOOD: works for non-copyable types too
template<typename T>
void log(const T& value) {
    std::cout << value << '\n';
}
```

### Don't Replace Runtime Checks

Concepts are **compile-time**. For runtime validation, use assertions:

```cpp
// Can't check at compile time if value > 0
template<std::integral T>
void process_positive(T value) {
    assert(value > 0);  // runtime check
    // ...
}
```

---

## 9.14 Concepts in agentty: Survey

Here's where concepts are used in the codebase:

| Location | Concept | Purpose |
|----------|---------|---------|
| `core/ids.hpp` | `IdTag` | Ensure tags have a `name` |
| `provider/concept.hpp` | `Provider` | Streaming interface contract |
| `concurrency/ranked_mutex.hpp` | `Lockable` | Ensure mutex-like types |
| `tool/constraints.hpp` | `ToolExecutor` | Tool calling interface |
| `util/hash.hpp` | `Hashable` | Types with std::hash support |

**Pattern:** Concepts are used at **seam points** — where different subsystems meet and need a clear contract.

---

## 9.15 Summary

**What we learned:**
- ✅ Concepts constrain template parameters with named requirements
- ✅ Standard library provides dozens of useful concepts
- ✅ Custom concepts document APIs and clarify errors
- ✅ Concepts enable constraint-based overload resolution
- ✅ Concepts are zero-cost (compile-time only)
- ✅ agentty uses concepts at subsystem boundaries

**Key takeaway:** Concepts turn vague template duck typing into **explicit, checkable contracts**.

---

## 9.16 Exercises

### Exercise 1: Define a Printable Concept

Write a concept `Printable` that checks if a type can be output to `std::ostream`:

```cpp
template<typename T>
concept Printable = /* YOUR CODE */;

// Should satisfy:
static_assert(Printable<int>);
static_assert(Printable<std::string>);
static_assert(!Printable<std::mutex>);
```

**Hint:** Use `requires` with `std::cout << value`.

<details>
<summary>Solution</summary>

```cpp
template<typename T>
concept Printable = requires(std::ostream& os, const T& value) {
    { os << value } -> std::convertible_to<std::ostream&>;
};
```

</details>

---

### Exercise 2: Constrain a Generic Find Function

Write a `find()` function that works on any **container** with `begin()`, `end()`, and a `value_type`:

```cpp
template</* YOUR CONCEPT */ C, typename T>
auto find(const C& container, const T& value) {
    // Use std::find
}
```

**Constraints:**
- `C` must have `begin()` and `end()`
- `C::value_type` must exist
- `T` must be comparable to `C::value_type`

<details>
<summary>Solution</summary>

```cpp
template<typename C>
concept Container = requires(C c) {
    typename C::value_type;
    { c.begin() };
    { c.end() };
};

template<Container C, typename T>
requires std::equality_comparable_with<T, typename C::value_type>
auto find(const C& container, const T& value) {
    return std::find(container.begin(), container.end(), value);
}
```

</details>

---

### Exercise 3: Refine Numeric Concept

Write two concepts:

1. `Numeric` — supports +, -, *, /
2. `StrictNumeric` — additionally requires `std::is_arithmetic_v<T>`

Test with `int`, `double`, and a custom `BigInt` class.

<details>
<summary>Solution</summary>

```cpp
template<typename T>
concept Numeric = requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;
    { a - b } -> std::convertible_to<T>;
    { a * b } -> std::convertible_to<T>;
    { a / b } -> std::convertible_to<T>;
};

template<typename T>
concept StrictNumeric = Numeric<T> && std::is_arithmetic_v<T>;

// Usage:
class BigInt {
    // implements +, -, *, /
};

static_assert(Numeric<int>);          // ✅
static_assert(StrictNumeric<int>);    // ✅
static_assert(Numeric<BigInt>);       // ✅
static_assert(!StrictNumeric<BigInt>); // ❌ (not arithmetic)
```

</details>

---

### Exercise 4: Provider Concept for agentty

Write a concept `StreamProvider` that requires:

- A `stream(Request, EventSink)` method returning `StreamResult`
- A `name()` method returning string-like
- A `supports_images()` method returning `bool`

Then write a generic `log_provider_info()` function constrained by this concept.

<details>
<summary>Solution</summary>

```cpp
template<typename P>
concept StreamProvider = requires(P p, const Request& req, EventSink sink) {
    { p.stream(req, sink) } -> std::same_as<StreamResult>;
    { p.name() } -> std::convertible_to<std::string_view>;
    { p.supports_images() } -> std::same_as<bool>;
};

template<StreamProvider P>
void log_provider_info(const P& provider) {
    std::cout << "Provider: " << provider.name() << '\n';
    std::cout << "Images: " << (provider.supports_images() ? "yes" : "no") << '\n';
}
```

</details>

---

### Exercise 5: Overload on Constraint Strength

Write three `process()` overloads:

1. Generic (any type)
2. `std::integral` types
3. `std::signed_integral` types

Test with `int`, `unsigned`, and `std::string`. Which overload is picked for each?

<details>
<summary>Solution</summary>

```cpp
template<typename T>
void process(T value) {
    std::cout << "Generic: " << value << '\n';
}

template<std::integral T>
void process(T value) {
    std::cout << "Integral: " << value << '\n';
}

template<std::signed_integral T>
void process(T value) {
    std::cout << "Signed integral: " << value << '\n';
}

// Tests:
process(std::string("hi"));  // Generic (only match)
process(42u);                // Integral (more specific than Generic)
process(-5);                 // Signed integral (most specific)
```

**Rule:** Most constrained overload wins.

</details>

---

## Next Chapter

In [Chapter 10: constexpr and Compile-Time Computation](../ch10-constexpr/README.md), we'll see how to move computation from runtime to compile time using `constexpr`, `consteval`, and `constinit`.

**Preview:** Did you know agentty proves its tool catalog is complete **at compile time**? We'll build that proof.

---

## Further Reading

- [C++20 Concepts (cppreference)](https://en.cppreference.com/w/cpp/language/constraints)
- [Concepts TS Specification](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/n4674.pdf)
- agentty source: `include/agentty/provider/concept.hpp`
- agentty source: `include/agentty/core/ids.hpp`

---

**Previous:** [Chapter 8: std::visit and Pattern Matching](../ch08-visit/README.md)  
**Next:** [Chapter 10: constexpr and Compile-Time Computation](../ch10-constexpr/README.md)  
**Up:** [Part II: Modern C++ Patterns](../README.md)
