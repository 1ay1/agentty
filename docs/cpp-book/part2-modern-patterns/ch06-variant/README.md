# Chapter 6: std::variant and Sum Types

**Goal:** Master algebraic data types and exhaustive pattern matching.

**Time:** 4-5 hours  
**Prerequisites:** Chapters 1-5

---

## 6.1 What Are Sum Types?

In type theory, there are two ways to combine types:

### Product Types (structs/classes)

**A AND B** — contains both fields simultaneously:

```cpp
struct Message {
    MessageId id;      // Has an id AND
    Role role;         // Has a role AND
    std::string text;  // Has text
};
```

**Cardinality:** `|Message| = |MessageId| × |Role| × |string|`

### Sum Types (variants/unions)

**A OR B** — contains exactly one alternative at a time:

```cpp
std::variant<int, float, std::string> value;
// value is EITHER an int OR a float OR a string (never two simultaneously)
```

**Cardinality:** `|variant<A,B,C>| = |A| + |B| + |C|`

### Why "Sum" and "Product"?

**Product example:** 
```cpp
struct Point { int x; int y; };
// |Point| = 2^32 × 2^32 = 2^64 possible values
```

**Sum example:**
```cpp
std::variant<bool, bool> v;
// |v| = 2 + 2 = 4 possible values
// (true, left) | (false, left) | (true, right) | (false, right)
```

---

## 6.2 std::variant Basics

### Declaration and Initialization

```cpp
#include <variant>

std::variant<int, float, std::string> value;
// Default constructs first alternative: int (value == 0)

value = 42;         // Now holds int
value = 3.14f;      // Now holds float
value = "hello";    // Now holds std::string
```

### Accessing the Value

**Pattern 1: std::get<T> (throws if wrong type)**
```cpp
std::variant<int, std::string> v = 42;

int i = std::get<int>(v);        // OK: returns 42
std::string s = std::get<std::string>(v);  // THROWS std::bad_variant_access
```

**Pattern 2: std::get<Index> (by position)**
```cpp
std::variant<int, float> v = 3.14f;

float f = std::get<1>(v);  // OK: second alternative
int i = std::get<0>(v);    // THROWS: first alternative
```

**Pattern 3: std::get_if (returns pointer, nullptr if wrong)**
```cpp
std::variant<int, std::string> v = 42;

if (int* p = std::get_if<int>(&v)) {
    std::cout << "int: " << *p;
} else {
    std::cout << "not an int";
}
```

**Pattern 4: std::holds_alternative (check without extracting)**
```cpp
std::variant<int, std::string> v = "hello";

if (std::holds_alternative<std::string>(v)) {
    auto& s = std::get<std::string>(v);  // Safe, no throw
    std::cout << s;
}
```

### Real Example from agentty: Tool State

```cpp
// include/agentty/domain/conversation.hpp
struct ToolUse {
    struct Queued {};
    
    struct Executing {
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point last_progress_at;
        std::string partial_output;
    };
    
    struct Done {
        std::string output;
        std::vector<ImageContent> images;
        int exit_code;
    };
    
    struct Failed {
        std::string error;
    };
    
    using State = std::variant<Queued, Executing, Done, Failed>;
    
    ToolCallId id;
    std::string name;
    json args;
    State state;
};
```

**Usage:**
```cpp
ToolUse tool = create_tool_call();
// tool.state is Queued

dispatch_tool(tool);
// tool.state transitions to Executing

// Later...
if (auto* done = std::get_if<ToolUse::Done>(&tool.state)) {
    process_output(done->output);
} else if (auto* failed = std::get_if<ToolUse::Failed>(&tool.state)) {
    log_error(failed->error);
}
```

---

## 6.3 Visiting Variants: Exhaustive Pattern Matching

### The Problem: Switching on Type

```cpp
// Bad: manual type checking
if (std::holds_alternative<int>(v)) {
    auto x = std::get<int>(v);
    // ...
} else if (std::holds_alternative<float>(v)) {
    auto x = std::get<float>(v);
    // ...
} else if (std::holds_alternative<std::string>(v)) {
    auto x = std::get<std::string>(v);
    // ...
}
// Easy to forget a case!
```

### The Solution: std::visit

```cpp
std::variant<int, float, std::string> v = 42;

std::visit([](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, int>) {
        std::cout << "int: " << arg;
    } else if constexpr (std::is_same_v<T, float>) {
        std::cout << "float: " << arg;
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::cout << "string: " << arg;
    }
}, v);
```

**Key insight:** `std::visit` ensures ALL cases are handled. If you add a new alternative to the variant, EVERY visit must be updated or it won't compile.

### The overload Helper (from maya)

```cpp
// maya/include/maya/core/overload.hpp
template <typename... Fs>
struct overload : Fs... { 
    using Fs::operator()...; 
};
```

**Usage:**
```cpp
std::variant<int, float, std::string> v = "hello";

std::visit(overload{
    [](int x)               { std::cout << "int: " << x; },
    [](float x)             { std::cout << "float: " << x; },
    [](const std::string& x) { std::cout << "string: " << x; },
}, v);
```

**How it works:**
1. `overload<F1, F2, F3>` inherits from F1, F2, F3
2. `using Fs::operator()...` brings all `operator()` into scope
3. Overload resolution picks the right one based on argument type

### Real Example from agentty: Message Dispatch

```cpp
// src/runtime/app/update.cpp
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    return std::visit(overload{
        [&](msg::ComposerMsg cm)   { return detail::composer_update(std::move(m), std::move(cm)); },
        [&](msg::StreamMsg sm)     { return detail::stream_update(std::move(m), std::move(sm)); },
        [&](msg::ThreadListMsg tm) { return detail::thread_list_update(std::move(m), std::move(tm)); },
        [&](msg::PickerMsg pm)     { return detail::picker_update(std::move(m), std::move(pm)); },
        [&](msg::LoginMsg lm)      { return detail::login_update(std::move(m), std::move(lm)); },
        [&](msg::DiffReviewMsg dm) { return detail::diff_review_update(std::move(m), std::move(dm)); },
        [&](msg::SmartModeMsg sm)  { return detail::smart_mode_update(std::move(m), std::move(sm)); },
        [&](msg::PluginEditMsg pm) { return detail::plugin_edit_update(std::move(m), std::move(pm)); },
        [&](msg::AppearanceMsg am) { return detail::appearance_update(std::move(m), std::move(am)); },
        [&](msg::MetaMsg mm)       { return detail::meta_update(std::move(m), std::move(mm)); },
    }, msg);
}
```

**Why this is brilliant:**
1. **Exhaustive** — Adding new Msg type without handler = compile error
2. **Type-safe** — Each handler receives the exact type it expects
3. **Fast** — std::visit compiles to a jump table (O(1) dispatch)
4. **Maintainable** — All message handling in one place

---

## 6.4 Real Example: agentty's Msg System

### The Problem

agentty has 200+ message types. Organizing them in one giant variant has problems:

```cpp
// BAD: One flat variant
using Msg = std::variant<
    ComposerEnter,
    ComposerBackspace,
    ComposerSubmit,
    StreamTextDelta,
    StreamToolUse,
    StreamFinished,
    ThreadListOpen,
    ThreadListMove,
    // ... 190 more types
>;
```

**Problems:**
1. **sizeof(Msg)** is pinned by the LARGEST alternative (even if you never use it)
2. **Compile time** — every change rebuilds everything
3. **Dispatch** — 200-arm jump table

### The Solution: Nested Variants

```cpp
// include/agentty/runtime/msg.hpp

// Domain sub-variants
namespace msg {
    using ComposerMsg = std::variant<
        ComposerEnter,
        ComposerBackspace,
        ComposerSubmit,
        ComposerPaste,
        // ... 10 composer messages
    >;
    
    using StreamMsg = std::variant<
        StreamTextDelta,
        StreamToolUse,
        StreamFinished,
        StreamError,
        // ... 8 stream messages
    >;
    
    using ThreadListMsg = std::variant<
        ThreadListOpen,
        ThreadListMove,
        ThreadListSelect,
        // ... 6 thread list messages
    >;
    
    // 7 more domain variants...
}

// Top-level variant (10 alternatives instead of 200)
using Msg = std::variant<
    msg::ComposerMsg,
    msg::StreamMsg,
    msg::ThreadListMsg,
    msg::PickerMsg,
    msg::LoginMsg,
    msg::DiffReviewMsg,
    msg::SmartModeMsg,
    msg::PluginEditMsg,
    msg::AppearanceMsg,
    msg::MetaMsg
>;
```

**Benefits:**
1. **Smaller sizeof** — Each domain variant is only as large as its largest member
2. **Faster compilation** — Changing a ComposerMsg only rebuilds composer code
3. **Two-level dispatch** — First pick domain (10-way), then pick message within domain (10-way avg)

### Dispatching Nested Variants

```cpp
// Top-level dispatch picks domain
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    return std::visit(overload{
        [&](msg::ComposerMsg cm) {
            // Second-level dispatch within composer domain
            return std::visit(overload{
                [&](ComposerEnter) -> std::pair<Model, Cmd<Msg>> {
                    // Handle enter
                },
                [&](ComposerBackspace) -> std::pair<Model, Cmd<Msg>> {
                    // Handle backspace
                },
                // ...
            }, cm);
        },
        [&](msg::StreamMsg sm) {
            return std::visit(overload{
                [&](StreamTextDelta delta) -> std::pair<Model, Cmd<Msg>> {
                    m.stream.text += delta.text;
                    return {std::move(m), Cmd<Msg>::none()};
                },
                // ...
            }, sm);
        },
        // ... other domains
    }, msg);
}
```

**In practice:** Each domain has its own file (`update/composer.cpp`, `update/stream.cpp`) so the second-level dispatch is hidden.

---

## 6.5 Advanced: std::variant Implementation Details

### Memory Layout

```cpp
std::variant<int, double, std::string> v;
// sizeof(v) >= max(sizeof(int), sizeof(double), sizeof(std::string)) + discriminator
```

The variant stores:
1. **The active alternative** (largest alternative's size)
2. **A discriminator** (1-4 bytes, which alternative is active)

**Example:**
```cpp
sizeof(int) = 4
sizeof(double) = 8
sizeof(std::string) = 24 (typically)

sizeof(std::variant<int, double, std::string>) = 24 + 4 (padding) = 32
```

### valueless_by_exception

```cpp
struct ThrowsInMove {
    ThrowsInMove(ThrowsInMove&&) { throw std::runtime_error("oops"); }
};

std::variant<int, ThrowsInMove> v = 42;
try {
    v = ThrowsInMove{};  // Move constructor throws
} catch (...) {
    // v is now valueless_by_exception
}

std::cout << v.valueless_by_exception();  // true
std::cout << v.index();  // std::variant_npos
```

**How to avoid:** Use noexcept move constructors (best practice anyway).

### Variant vs. Union

**Old C union (unsafe):**
```cpp
union U {
    int i;
    float f;
    char* str;
};

U u;
u.i = 42;
float f = u.f;  // UB: reading wrong member
```

**std::variant (safe):**
```cpp
std::variant<int, float, const char*> v = 42;
float f = std::get<float>(v);  // EXCEPTION: wrong alternative
```

---

## 6.6 Exercises

### Exercise 6.1: Basic Variant Usage

Implement a calculator that stores results as `int`, `double`, or `std::string` (for errors):

```cpp
using Result = std::variant<int, double, std::string>;

Result add(Result a, Result b);
Result multiply(Result a, Result b);
void print_result(const Result& r);
```

**Starter code:** `exercises/ch06/ex1-calculator.cpp`  
**Solution:** `solutions/ch06/ex1-calculator.cpp`

### Exercise 6.2: State Machine

Implement a TCP connection state machine using variants:

```cpp
struct Closed {};
struct Connecting { std::string host; int port; };
struct Connected { int socket_fd; };
struct Error { std::string message; };

using State = std::variant<Closed, Connecting, Connected, Error>;

class Connection {
    State state_;
public:
    void connect(std::string host, int port);
    void on_connected(int fd);
    void on_error(std::string msg);
    void close();
};
```

**Starter code:** `exercises/ch06/ex2-state-machine.cpp`  
**Solution:** `solutions/ch06/ex2-state-machine.cpp`

### Exercise 6.3: Mini Message System

Implement a simplified version of agentty's message system:

```cpp
// Three domains: Input, Network, UI
namespace msg {
    using InputMsg = std::variant<KeyPress, MouseClick>;
    using NetworkMsg = std::variant<DataReceived, ConnectionClosed>;
    using UIMsg = std::variant<Redraw, Resize>;
}

using Msg = std::variant<msg::InputMsg, msg::NetworkMsg, msg::UIMsg>;

struct Model {
    std::string buffer;
    bool connected;
};

std::pair<Model, std::vector<std::string>> update(Model m, Msg msg);
```

**Starter code:** `exercises/ch06/ex3-mini-messages.cpp`  
**Solution:** `solutions/ch06/ex3-mini-messages.cpp`

### Exercise 6.4: Implement overload

Write your own version of the `overload` helper:

```cpp
template <typename... Fs>
struct my_overload : /* ??? */ {
    // Your implementation
};

// Test:
std::variant<int, std::string> v = 42;
std::visit(my_overload{
    [](int x) { std::cout << "int: " << x; },
    [](const std::string& s) { std::cout << "string: " << s; },
}, v);
```

**Starter code:** `exercises/ch06/ex4-overload.cpp`  
**Solution:** `solutions/ch06/ex4-overload.cpp`

---

## Key Takeaways

1. **std::variant is a type-safe union**
   - Exactly one alternative active at a time
   - Compiler tracks which one

2. **std::visit provides exhaustive pattern matching**
   - Must handle all cases
   - Adding new alternative forces every visit to update

3. **overload helper enables clean syntax**
   - One lambda per alternative
   - Reads like match expression

4. **Nested variants reduce compile time**
   - agentty's 10-domain design
   - sizeof(Msg) smaller
   - Faster compilation

5. **Variants enable state machines**
   - Each state is a distinct type
   - Illegal transitions = compile errors

---

## Next Chapter

[Chapter 7: std::expected and Monadic Error Handling →](../ch07-expected/README.md)

In the next chapter, you'll learn:
- Why exceptions are problematic
- std::expected<T, E> for error handling
- Monadic operations: and_then, or_else, transform
- How maya's Result<T> works
