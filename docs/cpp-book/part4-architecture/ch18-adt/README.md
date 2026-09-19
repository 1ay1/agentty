# Chapter 18: Algebraic Data Types

**Learning Objectives:**
- Understand product types (structs, tuples)
- Master sum types (variants, enums)
- Build recursive data structures
- Ensure exhaustive pattern matching
- See how agentty models complex state
- Prove correctness with the type system

---

## 18.1 What Are Algebraic Data Types?

### Types as Algebra

**Product type:** `A AND B` (both fields present)

```cpp
struct Point {
    int x;  // AND
    int y;
};
```

**Sum type:** `A OR B` (one variant present)

```cpp
std::variant<int, std::string>  // int OR string
```

**Why "algebraic"?**

Count the number of possible values:

```cpp
struct Bool2 { bool a; bool b; };
// Possible values: (false,false), (false,true), (true,false), (true,true)
// Count: 2 × 2 = 4  (product!)

std::variant<bool, bool>
// Possible values: bool(false), bool(true), bool(false), bool(true)
// Count: 2 + 2 = 4  (sum!)
```

**Math:**
- Product type: `|A × B| = |A| × |B|`
- Sum type: `|A + B| = |A| + |B|`

---

## 18.2 Product Types

### Structs

```cpp
struct User {
    std::string name;
    int age;
    std::string email;
};
```

**All fields must be present.**

### Tuples

```cpp
std::tuple<std::string, int, std::string> user = {"Alice", 30, "alice@example.com"};
```

**Tuples are anonymous structs.**

### Named vs Anonymous

```cpp
// Named (preferred for domain modeling)
struct Point { int x; int y; };

// Anonymous (preferred for utility)
std::pair<int, int>
```

---

## 18.3 Sum Types

### std::variant

```cpp
std::variant<int, double, std::string> value;
value = 42;        // Holds int
value = 3.14;      // Holds double
value = "hello";   // Holds string
```

**Exactly one variant is active at a time.**

### Exhaustive Matching

```cpp
std::visit(overload{
    [](int x) { std::cout << "int: " << x; },
    [](double x) { std::cout << "double: " << x; },
    [](std::string x) { std::cout << "string: " << x; }
}, value);
```

**If you forget a case:** Compile error.

---

## 18.4 Real Example: agentty's Msg Type

### The Type

`agentty/include/agentty/runtime/msg.hpp`:

```cpp
using Msg = std::variant<
    // Composer events
    ComposerEnter,
    ComposerExit,
    ComposerInput,
    
    // Streaming events
    StreamStart,
    StreamTextDelta,
    StreamToolCall,
    StreamEnd,
    
    // Tool execution
    ExecuteTool,
    ToolComplete,
    ToolFailed,
    
    // UI events
    ScrollUp,
    ScrollDown,
    ThreadSwitch,
    
    // ... 60+ variants
>;
```

**Why variant?**
- **Type safety:** Can't mix up message types
- **Exhaustiveness:** Compiler ensures all cases handled
- **Performance:** No heap allocation (inline storage)

---

## 18.5 Recursive Types

### The Problem

Model a tree:

```cpp
struct Tree {
    int value;
    Tree* left;   // Pointer required (recursive)
    Tree* right;
};
```

**Why pointer?** Can't have infinite size.

### With std::unique_ptr

```cpp
struct Tree {
    int value;
    std::unique_ptr<Tree> left;
    std::unique_ptr<Tree> right;
};
```

**Now:**
- Ownership is clear (unique_ptr)
- No manual delete needed (RAII)

### With std::variant (Sum Type)

```cpp
struct Leaf { int value; };
struct Node {
    int value;
    std::unique_ptr<Tree> left;
    std::unique_ptr<Tree> right;
};

using Tree = std::variant<Leaf, Node>;
```

**Benefits:**
- Leaf and Node are **different types**
- Pattern matching distinguishes them

---

## 18.6 Real Example: agentty's Thread Structure

### The Model

A thread is a **tree** of messages (forking creates branches):

```cpp
struct Message {
    MessageId id;
    std::string content;
    std::vector<MessageId> children;  // Forked threads
};

struct Thread {
    ThreadId id;
    std::vector<Message> messages;
    std::optional<ThreadId> parent;  // Forked from
};
```

**Operations:**
1. Add message (append to current branch)
2. Fork thread (create child with new branch)
3. Navigate history (walk tree)

---

## 18.7 Pattern: Optional Values

### std::optional as Sum Type

```cpp
std::optional<int> maybe_value;

// Two variants:
// - std::nullopt (nothing)
// - int value (something)
```

**Algebraically:**

```cpp
std::optional<T> ≈ std::variant<std::monostate, T>
```

### Safe Unwrapping

```cpp
// Bad: unchecked
int x = *maybe_value;  // Undefined behavior if nullopt

// Good: checked
if (maybe_value.has_value()) {
    int x = *maybe_value;
}

// Better: pattern match
maybe_value.transform([](int x) {
    std::cout << x;
});
```

---

## 18.8 Pattern: Result Types

### std::expected<T, E>

```cpp
std::expected<std::string, Error> read_file(std::string path) {
    if (auto f = open(path)) {
        return f.read();  // Success
    }
    return std::unexpected{Error::FileNotFound};  // Failure
}
```

**Sum type:**

```cpp
std::expected<T, E> ≈ std::variant<T, E>
```

**Benefits:**
- Forces error handling
- No exceptions
- Errors in type signature

---

## 18.9 Designing with Sum Types

### Example: File System Entry

```cpp
struct File {
    std::string name;
    size_t size;
    std::vector<uint8_t> content;
};

struct Directory {
    std::string name;
    std::vector<Entry> children;
};

using Entry = std::variant<File, Directory>;
```

**Usage:**

```cpp
size_t compute_size(const Entry& entry) {
    return std::visit(overload{
        [](const File& f) { return f.size; },
        [](const Directory& d) {
            size_t total = 0;
            for (auto& child : d.children) {
                total += compute_size(child);  // Recursive
            }
            return total;
        }
    }, entry);
}
```

---

## 18.10 Ensuring Exhaustiveness

### The Compiler as Proof Checker

```cpp
enum class Color { Red, Green, Blue };

std::string name(Color c) {
    switch (c) {
        case Color::Red: return "red";
        case Color::Green: return "green";
        // Missing Blue!
    }
}
```

**Without warning:** Runtime undefined behavior.

**With `-Wswitch` (GCC/Clang):** Compile-time error:

```
warning: enumeration value 'Blue' not handled in switch [-Wswitch]
```

**Better: use std::variant**

```cpp
using Color = std::variant<Red, Green, Blue>;

std::string name(const Color& c) {
    return std::visit(overload{
        [](Red) { return "red"; },
        [](Green) { return "green"; }
        // Forgot Blue — won't compile
    }, c);
}
```

---

## 18.11 Real Example: agentty's StreamResult

### The Type

`agentty/include/agentty/provider/stream_result.hpp`:

```cpp
struct StreamResult {
    enum class Status { Ok, Cancelled, RateLimited, ServerError };
    
    Status status;
    std::optional<std::string> error_message;
    std::optional<int> retry_after_seconds;
    
    static StreamResult ok() {
        return {Status::Ok, std::nullopt, std::nullopt};
    }
    
    static StreamResult failed(std::string msg) {
        return {Status::ServerError, std::move(msg), std::nullopt};
    }
    
    static StreamResult rate_limited(int retry_after) {
        return {Status::RateLimited, std::nullopt, retry_after};
    }
};
```

**Pattern matching:**

```cpp
void handle_result(StreamResult result) {
    switch (result.status) {
        case StreamResult::Status::Ok:
            // Success
            break;
        case StreamResult::Status::Cancelled:
            // User cancelled
            break;
        case StreamResult::Status::RateLimited:
            // Retry after result.retry_after_seconds
            break;
        case StreamResult::Status::ServerError:
            // Show result.error_message
            break;
    }
}
```

---

## 18.12 Type-Level State Machines

### Example: Connection States

```cpp
struct Disconnected {};
struct Connecting { std::string url; };
struct Connected { int socket_fd; };
struct Failed { std::string error; };

using ConnectionState = std::variant<
    Disconnected,
    Connecting,
    Connected,
    Failed
>;
```

**Type-safe transitions:**

```cpp
ConnectionState connect(Disconnected, std::string url) {
    return Connecting{url};  // Can only connect from Disconnected
}

ConnectionState complete(Connecting c, int fd) {
    return Connected{fd};  // Can only complete from Connecting
}
```

**Illegal state is unrepresentable:**

```cpp
// Can't connect from Connected (won't compile)
// ConnectionState connect(Connected, std::string url);
```

---

## 18.13 Summary

**What we learned:**
- ✅ Product types = AND (structs, tuples)
- ✅ Sum types = OR (variants, enums)
- ✅ Recursive types need pointers/unique_ptr
- ✅ Pattern matching ensures exhaustiveness
- ✅ optional and expected are sum types
- ✅ Sum types model state machines safely

**Key insight:** Make illegal states **unrepresentable** with the type system.

---

## 18.14 Exercises

### Exercise 1: Model a Shape

Create a sum type for shapes:

```cpp
struct Circle { double radius; };
struct Rectangle { double width; double height; };
struct Triangle { double base; double height; };

using Shape = /* YOUR CODE */;

double area(const Shape& s) {
    // YOUR CODE
}
```

---

### Exercise 2: Binary Tree

```cpp
struct Leaf { int value; };
struct Node {
    int value;
    std::unique_ptr<Tree> left;
    std::unique_ptr<Tree> right;
};

using Tree = std::variant<Leaf, Node>;

int sum(const Tree& t) {
    // YOUR CODE
}
```

---

### Exercise 3: State Machine

Model a door that can be Open, Closed, or Locked:

```cpp
struct Open {};
struct Closed {};
struct Locked { std::string key; };

using DoorState = std::variant<Open, Closed, Locked>;

// Transitions
DoorState close(Open);
DoorState lock(Closed, std::string key);
DoorState unlock(Locked, std::string key);
```

---

## Next Chapter

In [Chapter 19: Dependency Injection via Type Erasure](../ch19-di/README.md), we'll see how to test code with external dependencies.

---

**Previous:** [Chapter 17: Effect Systems](../ch17-effects/README.md)  
**Next:** [Chapter 19: Dependency Injection](../ch19-di/README.md)  
**Up:** [Part IV: Architecture](../README.md)
