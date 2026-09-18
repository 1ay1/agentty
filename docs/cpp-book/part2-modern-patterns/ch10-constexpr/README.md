# Chapter 10: constexpr and Compile-Time Computation

**Learning Objectives:**
- Understand compile-time vs runtime execution
- Write `constexpr` functions that can run at either time
- Use `consteval` to require compile-time evaluation
- Apply `constinit` for safe static initialization
- See how agentty proves invariants at compile time
- Measure the cost/benefit of compile-time computation

---

## 10.1 Why Compile-Time Computation?

### The Promise

Move work from **runtime** (when the program runs) to **compile-time** (when it's built):

- ❌ Runtime: user waits for computation
- ✅ Compile time: computation is **free** at runtime

**Example:**

```cpp
// Runtime computation
int factorial_runtime(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}

auto x = factorial_runtime(10);  // Computed every time program runs

// Compile-time computation
constexpr int factorial_ct(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}

constexpr auto x = factorial_ct(10);  // Computed once at compile time
```

**At runtime:** `x` is just a constant (3628800), not a function call.

### When to Use Compile-Time Computation

**Good candidates:**
- Configuration constants (port numbers, buffer sizes)
- Lookup tables (CRC, trigonometry)
- Validation of invariants (catalog completeness, permission matrices)
- Type computations (metaprogramming)

**Bad candidates:**
- User input (unknown at compile time)
- File I/O (not allowed in constexpr)
- Large precomputation (increases compile time)

---

## 10.2 constexpr Functions

### Basic constexpr

A `constexpr` function **can** run at compile time **if** its inputs are compile-time constants:

```cpp
constexpr int square(int x) {
    return x * x;
}

// Compile-time evaluation
constexpr int a = square(5);  // Computed at compile time

// Runtime evaluation (input not constant)
int b = 10;
int c = square(b);  // Computed at runtime
```

**The rule:** `constexpr` means "**may** be compile-time," not "**must** be."

### Allowed Operations in constexpr (C++20)

C++20 greatly relaxed restrictions. Now allowed:

- ✅ Loops (`for`, `while`)
- ✅ Branching (`if`, `switch`)
- ✅ Local variables
- ✅ Calling other constexpr functions
- ✅ `std::string`, `std::vector` (transient allocation)
- ✅ Virtual functions (C++20)
- ✅ `new`/`delete` (must be matched)

**Still forbidden:**
- ❌ `asm` blocks
- ❌ Uninitialized variables
- ❌ `reinterpret_cast` (mostly)
- ❌ Goto
- ❌ Static variables with side effects

---

## 10.3 constexpr Variables

A `constexpr` variable is a **compile-time constant**:

```cpp
constexpr int max_threads = 8;
constexpr double pi = 3.14159265358979323846;
constexpr const char* version = "1.0.0";
```

**Difference from `const`:**

| Feature | `const` | `constexpr` |
|---------|---------|-------------|
| Initialized at | Runtime or compile-time | **Always** compile-time |
| Can be computed | Yes (via function call) | Only if function is `constexpr` |
| Usable in constant expressions | No (unless also constexpr) | ✅ Yes |

**Example:**

```cpp
const int a = get_value();      // OK (runtime)
constexpr int b = get_value();  // ERROR unless get_value() is constexpr
```

---

## 10.4 consteval: Immediate Functions

### The Problem

`constexpr` can fall back to runtime:

```cpp
constexpr int compute(int x) { return x * x; }

int y = 5;
int z = compute(y);  // Runs at RUNTIME (y not constant)
```

What if you **require** compile-time evaluation?

### The Solution: consteval

`consteval` means "**must** be compile-time":

```cpp
consteval int compute(int x) { return x * x; }

constexpr int a = compute(5);  // ✅ Compile-time
int b = compute(5);            // ✅ Compile-time (result stored)

int y = 5;
int z = compute(y);            // ❌ ERROR: y is not a constant expression
```

**Use cases:**
- Enforce compile-time validation
- Generate lookup tables
- Prove invariants (our use in agentty)

---

## 10.5 Real Example: agentty's Tool Catalog Proof

### The Problem

agentty exposes **20+ tools** to the AI (read, write, shell, grep, etc.). Each tool:

1. Has a **schema** (JSON description for the API)
2. Has an **executor** (C++ function that runs it)

**Invariant:** Every schema must have a corresponding executor, and vice versa.

**Challenge:** How do we **guarantee** this invariant?

### Naive Approach: Runtime Check

```cpp
void validate_catalog() {
    for (const auto& schema : tool_schemas) {
        if (!has_executor(schema.name)) {
            throw std::runtime_error("Missing executor for " + schema.name);
        }
    }
}

int main() {
    validate_catalog();  // Runtime check
    // ...
}
```

**Problems:**
- ❌ Only detected when program runs
- ❌ User can encounter the error
- ❌ Tests might not trigger all paths

### Better: Compile-Time Proof

`agentty/include/agentty/tool/catalog_proof.hpp`:

```cpp
// List of tool names (authoritative)
constexpr std::array<std::string_view, 23> TOOL_NAMES = {
    "read", "write", "edit", "shell", "grep", "find_definition",
    "search_code", "search_docs", "repo_map", "outline",
    "git_status", "git_diff", "git_log", "git_commit",
    "process_start", "process_poll", "process_stop",
    "web_fetch", "web_search", "remember", "forget",
    "task", "skill"
};

// Check if all names have schemas
consteval bool all_tools_have_schemas() {
    for (auto name : TOOL_NAMES) {
        if (!find_schema(name)) return false;
    }
    return true;
}

// Check if all names have executors
consteval bool all_tools_have_executors() {
    for (auto name : TOOL_NAMES) {
        if (!find_executor(name)) return false;
    }
    return true;
}

// PROOF: This must compile or the build fails
static_assert(all_tools_have_schemas(), 
              "Tool catalog incomplete: missing schema");
static_assert(all_tools_have_executors(), 
              "Tool catalog incomplete: missing executor");
```

**If you forget to implement a tool:** The build fails with a clear error, **before** the binary is created.

### The Power of consteval

- ❌ Can't accidentally fall back to runtime
- ✅ Forces compile-time checking
- ✅ Errors are immediate and actionable

---

## 10.6 constexpr and Containers

### C++20: std::vector and std::string are constexpr

You can now use dynamic containers in constexpr:

```cpp
constexpr auto make_vector() {
    std::vector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    return vec;
}

constexpr auto v = make_vector();  // Computed at compile time
```

**Constraints:**
- Allocation must happen **and be freed** within the constexpr context
- Can't return a pointer to allocated memory

**Example that works:**

```cpp
constexpr int sum_range(int n) {
    std::vector<int> vec(n);
    std::iota(vec.begin(), vec.end(), 1);  // Fill with 1, 2, ..., n
    return std::accumulate(vec.begin(), vec.end(), 0);
}

constexpr int total = sum_range(100);  // Computes 1+2+...+100 at compile time
```

### C++20: constexpr new/delete

You can allocate in constexpr, as long as you **clean up**:

```cpp
constexpr int compute() {
    int* p = new int(42);
    int result = *p * 2;
    delete p;  // Must match the new
    return result;
}

constexpr int value = compute();  // OK: allocation cleaned up
```

**Not allowed:**

```cpp
constexpr int* allocate() {
    return new int(42);  // ERROR: can't return allocated pointer
}
```

---

## 10.7 constinit: Safe Static Initialization

### The Problem: Static Initialization Order Fiasco

```cpp
// file1.cpp
int compute_value() { /* ... */ }
int global_a = compute_value();  // When is this initialized?

// file2.cpp
extern int global_a;
int global_b = global_a + 1;  // Undefined behavior if global_a not initialized yet
```

**Problem:** Initialization order of globals across translation units is **undefined**.

### The Solution: constinit

`constinit` guarantees **compile-time initialization**:

```cpp
constinit int global_value = 42;  // Initialized at compile time
```

**Rules:**
- Must be initialized with a constant expression
- Not `const` — can be modified at runtime
- Ensures initialization happens **before** any runtime code

**Example:**

```cpp
constexpr int compute() { return 42; }

constinit int value = compute();  // ✅ Compile-time init

int get_input();
constinit int bad = get_input();  // ❌ ERROR: not a constant expression
```

### Use in agentty

`agentty/src/runtime/config.cpp`:

```cpp
// Global config initialized at compile time
constinit Config g_default_config = {
    .max_threads = 8,
    .buffer_size = 4096,
    .timeout_ms = 30000,
};
```

**Benefits:**
- No static initialization order issues
- Guaranteed to be ready when `main()` starts
- Can still be modified if needed

---

## 10.8 Real Example: Compile-Time Lookup Tables

### Case Study: CRC32 Table

CRC32 needs a 256-entry lookup table. Instead of computing it at startup, compute it at **compile time**:

```cpp
constexpr uint32_t crc32_table_entry(uint8_t index) {
    uint32_t crc = index;
    for (int i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc;
}

constexpr auto make_crc32_table() {
    std::array<uint32_t, 256> table{};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = crc32_table_entry(static_cast<uint8_t>(i));
    }
    return table;
}

constexpr auto CRC32_TABLE = make_crc32_table();

// Usage: CRC32_TABLE is a compile-time constant
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ data[i]) & 0xFF];
    }
    return ~crc;
}
```

**At compile time:** The table is fully computed.  
**At runtime:** It's just a 1KB constant in `.rodata`.

**Cost:** Zero runtime cost. Tiny compile-time cost (milliseconds).

---

## 10.9 Compile-Time String Hashing

### Use Case: Fast String Dispatch

agentty needs to dispatch tool calls by name:

```cpp
if (name == "read") { /* ... */ }
else if (name == "write") { /* ... */ }
else if (name == "shell") { /* ... */ }
// 20+ more comparisons
```

**Better:** Hash the name at compile time, dispatch on integer:

```cpp
consteval uint64_t hash(std::string_view str) {
    uint64_t h = 0xcbf29ce484222325;  // FNV-1a offset
    for (char c : str) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3;           // FNV-1a prime
    }
    return h;
}

constexpr uint64_t READ_HASH = hash("read");
constexpr uint64_t WRITE_HASH = hash("write");
constexpr uint64_t SHELL_HASH = hash("shell");

void dispatch(std::string_view name) {
    switch (hash(name)) {  // Runtime hash
        case READ_HASH:   return do_read();
        case WRITE_HASH:  return do_write();
        case SHELL_HASH:  return do_shell();
        default:          return unknown_tool();
    }
}
```

**Why it's fast:**
- String hashes precomputed at compile time
- Runtime `switch` is a jump table (O(1))
- No string comparisons in the hot path

---

## 10.10 static_assert: Compile-Time Testing

`static_assert` enforces compile-time conditions:

```cpp
static_assert(sizeof(int) == 4, "Expected 32-bit int");
static_assert(std::is_trivially_copyable_v<Cell>, "Cell must be POD");
```

**If the assertion fails:** Compilation stops with the message.

### Real Example: Size Assertions in maya

`maya/include/maya/core/cell.hpp`:

```cpp
struct Cell {
    uint32_t codepoint;
    uint32_t style_id;
    // ...
};

// Prove it's exactly 8 bytes (for SIMD packing)
static_assert(sizeof(Cell) == 8, "Cell must be 8 bytes for SIMD");

// Prove it's trivially copyable (for memcpy)
static_assert(std::is_trivially_copyable_v<Cell>, 
              "Cell must be POD for direct memory operations");
```

**If someone adds a `std::string` to `Cell`:** Build fails immediately.

### Proving Type Safety

agentty's `Id<Tag>` system uses static_assert:

```cpp
template<typename Tag>
class Id {
    static_assert(requires { Tag::name; }, 
                  "Tag must have a 'name' member");
    // ...
};
```

**In C++20:** Concepts are better (Chapter 9), but `static_assert` works in C++17.

---

## 10.11 if constexpr: Compile-Time Branching

`if constexpr` chooses a branch at **compile time**:

```cpp
template<typename T>
void process(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        std::cout << "Integer: " << value << '\n';
    } else if constexpr (std::is_floating_point_v<T>) {
        std::cout << "Float: " << value << '\n';
    } else {
        std::cout << "Other: " << value << '\n';
    }
}

process(42);      // Only the first branch is compiled
process(3.14);    // Only the second branch is compiled
process("hello"); // Only the third branch is compiled
```

**Difference from runtime `if`:**

- Runtime `if`: **Both** branches must compile, one is chosen at runtime
- `if constexpr`: Only the **chosen** branch is compiled

**Use case:** Generic code that adapts to different types without template specialization.

---

## 10.12 Real Example: Type-Aware Serialization

agentty serializes various types to JSON:

```cpp
template<typename T>
nlohmann::json serialize(const T& value) {
    if constexpr (std::is_arithmetic_v<T>) {
        return value;  // Number
    } else if constexpr (std::is_same_v<T, std::string>) {
        return value;  // String
    } else if constexpr (requires { value.to_json(); }) {
        return value.to_json();  // Custom serialization
    } else {
        static_assert(always_false<T>, "Type not serializable");
    }
}
```

**Benefits:**
- Single generic function
- No runtime overhead (branch chosen at compile time)
- Clear error if type doesn't match

---

## 10.13 Measuring Compile-Time Cost

### The Tradeoff

Compile-time computation is **not free**:

- ✅ Zero runtime cost
- ❌ Increased compile time

**Example:** Computing a 1024-entry lookup table at compile time:

- Compile time: +50ms
- Runtime cost: 0µs (table is constant)

**Rule:** If the computation is done **once per run**, compile-time is usually worth it.

### Profiling Compile Time

Use compiler flags to measure:

```bash
# GCC/Clang
g++ -ftime-report myfile.cpp

# MSVC
cl /Bt myfile.cpp
```

Look for **constexpr evaluation time**.

### When NOT to Use Compile-Time

**Bad example:** Precomputing every possible input:

```cpp
// BAD: 1 million entries in binary
constexpr auto LOOKUP = make_table<1'000'000>();
```

- Binary size: +4MB
- Compile time: +30 seconds
- Benefit: Rarely used

**Better:** Compute on first use, cache in a static:

```cpp
const auto& get_table() {
    static auto table = make_table<1'000'000>();
    return table;
}
```

---

## 10.14 Practical Guidelines

### When to Use constexpr

✅ **Use for:**
- Configuration constants
- Small lookup tables (<10KB)
- Validation of invariants
- Type traits and metaprogramming

❌ **Avoid for:**
- Large data structures (bloats binary)
- User input (impossible)
- Rarely-used precomputation

### When to Use consteval

✅ **Use for:**
- Enforcing compile-time checks
- Building invariant proofs
- Generating compile-time IDs

❌ **Avoid for:**
- Anything that might need runtime fallback

### When to Use constinit

✅ **Use for:**
- Global configuration
- Avoiding static init order fiasco
- Constants that need to be mutable

❌ **Avoid for:**
- Local variables (unnecessary)
- Values that can't be compile-time computed

---

## 10.15 Summary

**What we learned:**
- ✅ `constexpr` functions can run at compile time or runtime
- ✅ `consteval` forces compile-time evaluation
- ✅ `constinit` ensures safe static initialization
- ✅ C++20 allows `std::vector`, `std::string`, and `new`/`delete` in constexpr
- ✅ `static_assert` and `consteval` enable compile-time proofs
- ✅ `if constexpr` chooses branches at compile time
- ✅ Compile-time has a cost (compile time, binary size)

**Key insight:** Move work from runtime to compile time when the cost is one-time and the benefit is every run.

---

## 10.16 Exercises

### Exercise 1: Compile-Time Factorial

Write a `constexpr` factorial function and use it to initialize a `constexpr` array of factorials from 0! to 10!.

```cpp
constexpr int factorial(int n) {
    // YOUR CODE
}

constexpr auto FACTORIALS = /* YOUR CODE */;

static_assert(FACTORIALS[5] == 120);
```

<details>
<summary>Solution</summary>

```cpp
constexpr int factorial(int n) {
    int result = 1;
    for (int i = 2; i <= n; ++i)
        result *= i;
    return result;
}

constexpr auto make_factorials() {
    std::array<int, 11> arr{};
    for (int i = 0; i <= 10; ++i)
        arr[i] = factorial(i);
    return arr;
}

constexpr auto FACTORIALS = make_factorials();

static_assert(FACTORIALS[0] == 1);
static_assert(FACTORIALS[5] == 120);
static_assert(FACTORIALS[10] == 3628800);
```

</details>

---

### Exercise 2: Compile-Time String Length

Write a `consteval` function that computes the length of a string literal and enforces compile-time evaluation:

```cpp
consteval size_t string_length(const char* str) {
    // YOUR CODE
}

constexpr auto len = string_length("hello");  // ✅
static_assert(len == 5);

const char* s = "world";
auto bad = string_length(s);  // Should ERROR
```

<details>
<summary>Solution</summary>

```cpp
consteval size_t string_length(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') ++len;
    return len;
}

constexpr auto len1 = string_length("hello");
static_assert(len1 == 5);

constexpr auto len2 = string_length("world!");
static_assert(len2 == 6);

// This would error (runtime pointer):
// const char* s = "test";
// auto bad = string_length(s);
```

</details>

---

### Exercise 3: Validate Tool Count at Compile Time

agentty has 23 tools. Write a `consteval` function that counts the tools in a list and proves (with `static_assert`) that it equals 23:

```cpp
constexpr std::array<std::string_view, /* ? */> TOOLS = {
    "read", "write", "edit", "shell", /* ... */
};

consteval size_t count_tools() {
    // YOUR CODE
}

static_assert(count_tools() == 23, "Expected 23 tools");
```

<details>
<summary>Solution</summary>

```cpp
constexpr std::array<std::string_view, 23> TOOLS = {
    "read", "write", "edit", "shell", "grep", "find_definition",
    "search_code", "search_docs", "repo_map", "outline",
    "git_status", "git_diff", "git_log", "git_commit",
    "process_start", "process_poll", "process_stop",
    "web_fetch", "web_search", "remember", "forget",
    "task", "skill"
};

consteval size_t count_tools() {
    return TOOLS.size();
}

static_assert(count_tools() == 23, "Expected 23 tools");
```

</details>

---

### Exercise 4: Compile-Time Prime Checker

Write a `constexpr` function `is_prime(int n)` that checks if n is prime. Use it to create a compile-time array of the first 10 primes.

<details>
<summary>Solution</summary>

```cpp
constexpr bool is_prime(int n) {
    if (n < 2) return false;
    for (int i = 2; i * i <= n; ++i) {
        if (n % i == 0) return false;
    }
    return true;
}

constexpr auto make_primes() {
    std::array<int, 10> primes{};
    int count = 0;
    int candidate = 2;
    while (count < 10) {
        if (is_prime(candidate)) {
            primes[count++] = candidate;
        }
        ++candidate;
    }
    return primes;
}

constexpr auto PRIMES = make_primes();

static_assert(PRIMES[0] == 2);
static_assert(PRIMES[9] == 29);
```

</details>

---

### Exercise 5: Prove Type Size with static_assert

agentty's `ThreadId` is an `Id<ThreadTag>`, which wraps a `std::string`. Prove that `sizeof(ThreadId) == sizeof(std::string)` (no overhead from the wrapper).

<details>
<summary>Solution</summary>

```cpp
struct ThreadTag {
    static constexpr std::string_view name = "thread";
};

template<typename Tag>
class Id {
    std::string value_;
public:
    explicit Id(std::string v) : value_(std::move(v)) {}
};

using ThreadId = Id<ThreadTag>;

static_assert(sizeof(ThreadId) == sizeof(std::string), 
              "ThreadId should have no overhead");
```

**Result:** Passes — the wrapper is truly zero-overhead.

</details>

---

### Exercise 6: if constexpr for Generic Print

Write a `print()` function that uses `if constexpr` to handle integers, floats, and strings differently:

```cpp
template<typename T>
void print(const T& value) {
    // Use if constexpr to:
    // - Print "int: X" for integers
    // - Print "float: X" for floats
    // - Print "string: X" for strings
}

print(42);       // "int: 42"
print(3.14);     // "float: 3.14"
print("hello");  // "string: hello"
```

<details>
<summary>Solution</summary>

```cpp
#include <iostream>
#include <type_traits>
#include <string_view>

template<typename T>
void print(const T& value) {
    if constexpr (std::is_integral_v<T>) {
        std::cout << "int: " << value << '\n';
    } else if constexpr (std::is_floating_point_v<T>) {
        std::cout << "float: " << value << '\n';
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        std::cout << "string: " << value << '\n';
    } else {
        std::cout << "unknown: " << value << '\n';
    }
}
```

</details>

---

## Next Chapter

In [Chapter 12: Template Metaprogramming](../../part3-advanced/ch12-metaprogramming/README.md), we'll see how to compute **types** at compile time, not just values.

**Preview:** Ever wondered how `std::vector` picks the right allocator based on element type? That's template metaprogramming.

---

## Further Reading

- [constexpr (cppreference)](https://en.cppreference.com/w/cpp/language/constexpr)
- [consteval (cppreference)](https://en.cppreference.com/w/cpp/language/consteval)
- [constinit (cppreference)](https://en.cppreference.com/w/cpp/language/constinit)
- agentty source: `include/agentty/tool/catalog_proof.hpp`
- agentty source: `src/runtime/config.cpp`

---

**Previous:** [Chapter 9: Concepts and Constraints](../ch09-concepts/README.md)  
**Next:** [Chapter 12: Template Metaprogramming](../../part3-advanced/ch12-metaprogramming/README.md)  
**Up:** [Part II: Modern C++ Patterns](../README.md)
