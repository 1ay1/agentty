# Chapter 1: C++ Basics — Types, Values, and References

**Goal:** Understand C++'s type system and why it enables compile-time safety.

**Time:** 2-3 hours  
**Prerequisites:** Basic programming (variables, functions, loops)

---

## 1.1 Why Types Matter

In dynamically-typed languages (Python, JavaScript), types are checked at runtime:

```python
# Python - runtime type error
thread_id = "abc123"
tool_id = "abc123"
save_thread(tool_id)  # Oops! Wrong ID, discovered when function runs
```

In C++, **types are contracts checked at compile time**:

```cpp
// C++ - compile-time type safety
ThreadId thread_id{"abc123"};
ToolCallId tool_id{"abc123"};
save_thread(tool_id);  // COMPILE ERROR: expected ThreadId, got ToolCallId
```

**Key insight:** Catch bugs when you write the code, not when users run it.

### The Cost of Runtime Type Errors

Real bug from agentty's history:
```cpp
// Old code - stringly typed
std::string fetch_model_id(const Request& r) {
    return r.provider_id;  // BUG: returned provider ID instead of model ID
}
// Used like:
auto id = fetch_model_id(req);
catalog.find_model(id);  // Returns nullopt, silent failure
```

**Impact:** User selects GPT-4, gets GPT-3.5. No error, just wrong behavior.

**Fix with strong types:**
```cpp
ModelId fetch_model_id(const Request& r) {
    return r.model_id;  // Can't accidentally return provider_id
}
// Type mismatch caught at compile time:
// catalog.find_model(provider_id);  // ERROR: expected ModelId, got ProviderId
```

### Creating Strong Types

**Template-based newtype pattern from agentty:**

```cpp
// include/agentty/domain/id.hpp
template <typename Tag>
struct Id {
    std::string value;
    
    // Explicit constructor prevents accidental conversions
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    
    // Comparison operators
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
};

// Create distinct types
using ThreadId   = Id<struct ThreadIdTag>;
using ToolCallId = Id<struct ToolCallIdTag>;
using ModelId    = Id<struct ModelIdTag>;
using MessageId  = Id<struct MessageIdTag>;
```

**Usage:**
```cpp
ThreadId tid{"thread_001"};
ToolCallId tcid{"tool_001"};

// This compiles:
process_thread(tid);

// This doesn't:
process_thread(tcid);  // ERROR: cannot convert ToolCallId to ThreadId
process_thread("thread_001");  // ERROR: explicit constructor
```

**Key features:**
1. **Zero runtime cost** — `sizeof(ThreadId) == sizeof(std::string)`
2. **Compile-time safety** — Wrong type = compile error
3. **Explicit construction** — No accidental conversions from strings
4. **Self-documenting** — Function signatures tell you what ID type they need

---

## 1.2 Values vs. References vs. Pointers

C++ has three ways to refer to data:

| Type | Syntax | Nullable? | Can rebind? | Ownership |
|------|--------|-----------|-------------|-----------|
| **Value** | `T x` | No | N/A | Owns the data |
| **Reference** | `T& x` | No | No | Aliases existing data |
| **Pointer** | `T* x` | Yes | Yes | Aliases existing data |

### Values: Owning Data

```cpp
std::string text = "hello";  // Value: owns 5 bytes of "hello"
std::string copy = text;     // Copy: new allocation, independent lifetime
copy[0] = 'H';               // Mutates copy, text unchanged
```

**When to use:**
- You need an independent copy
- The data is small (< 64 bytes)
- You're returning from a function (RVO optimization, Chapter 4)

**Real example from agentty:**
```cpp
// Domain types are values (cheap to copy or move)
struct Message {
    MessageId id;
    Role role;
    std::string text;
    std::vector<ImageContent> images;
    // ...
};

// Function returns by value (RVO elides the copy)
Message make_user_message(std::string text) {
    return Message{
        .id = generate_id(),
        .role = Role::User,
        .text = std::move(text)  // Move, not copy (Chapter 4)
    };
}
```

### References: Aliasing Data

```cpp
std::string text = "hello";
std::string& ref = text;     // Reference: ref is another name for text
ref[0] = 'H';                // Mutates text through ref
std::cout << text;           // Prints "Hello"
```

**Key properties:**
1. **Cannot be null** — Must refer to existing object
2. **Cannot be rebound** — Always refers to same object
3. **Zero overhead** — Compiled to pointer, but safer

**When to use:**
- Avoiding copies of large objects
- Modifying function arguments
- Returning subobjects from a larger object

**Real example from agentty:**
```cpp
class Model {
    Thread thread_;
    std::vector<Thread> history_;
    
public:
    // Read-only access: const reference
    const Thread& thread() const { 
        return thread_; 
    }
    
    // Read-write access: mutable reference
    Thread& thread_mut() { 
        return thread_; 
    }
    
    // Get specific history entry
    const Thread& history(std::size_t idx) const { 
        return history_.at(idx); 
    }
};

// Usage:
Model m = load_model();
const Thread& t = m.thread();  // No copy, just an alias
std::cout << t.messages.size();
```

**Why references, not pointers:**
- Cannot be null (no null checks needed)
- Intent is clearer ("I'm working with this object, not maybe-this-object")
- Safer (cannot be accidentally reassigned)

### Pointers: When You Need Nullable/Rebindable

```cpp
std::string* ptr = nullptr;      // Can be null
std::string text = "hello";
ptr = &text;                     // Can point to different objects

if (ptr != nullptr) {
    std::cout << *ptr;           // Dereference to access
}
```

**When to use:**
- Nullable (optional) references
- Polymorphism (base class pointers)
- Dynamic memory (but prefer smart pointers, Chapter 5)

**Real example from agentty:**
```cpp
// Tool execution can fail, so result is optional
struct ToolUse {
    ToolCallId id;
    std::string name;
    std::variant<Queued, Executing, Done, Failed> state;
    
    // Get output if done
    const std::string* output() const {
        if (auto* d = std::get_if<Done>(&state)) {
            return &d->output;
        }
        return nullptr;
    }
};

// Usage:
if (const std::string* out = tool.output()) {
    process(*out);
} else {
    // Still executing or failed
}
```

**Modern alternative: `std::optional`**
```cpp
std::optional<std::string_view> output() const {
    if (auto* d = std::get_if<Done>(&state)) {
        return d->output;
    }
    return std::nullopt;
}

// Usage:
if (auto out = tool.output()) {
    process(*out);
}
```

---

## 1.3 const Correctness

`const` is a **compile-time promise** that you won't modify something.

### const Variables

```cpp
const int x = 42;
x = 43;  // COMPILE ERROR: x is const
```

### const References (Read-Only Access)

```cpp
void print_thread(const Thread& t) {
    std::cout << t.title;       // OK: reading
    t.messages.clear();         // COMPILE ERROR: modifying const object
}
```

**Why use const references:**
1. **Avoids copies** — No allocation for large objects
2. **Documents intent** — "I won't modify this"
3. **Enables compiler optimizations** — const values can be cached
4. **Thread-safe** — Multiple threads can read const references simultaneously

**Real example from maya:**
```cpp
// Rendering is read-only — takes const Model
Element view(const Model& model) {
    return v(
        render_thread(model.thread()),
        render_composer(model.composer),
        render_statusbar(model.ui.status)
    );
}
```

### const Member Functions

```cpp
class Thread {
    std::string title_;
    std::vector<Message> messages_;
    
public:
    // const member function: promises not to modify `this`
    std::size_t message_count() const {
        return messages_.size();
    }
    
    // const overload: returns const reference
    const std::vector<Message>& messages() const {
        return messages_;
    }
    
    // Non-const overload: returns mutable reference
    std::vector<Message>& messages_mut() {
        return messages_;
    }
    
    // Mutating operation: not const
    void add_message(Message m) {
        messages_.push_back(std::move(m));
    }
};

// Usage:
const Thread& ct = get_thread();
ct.message_count();  // OK
ct.messages();       // Returns const reference
ct.add_message(msg); // COMPILE ERROR: ct is const

Thread& mt = get_thread_mut();
mt.add_message(msg);  // OK
```

**Key insight:** const-correctness propagates. If you have a const reference, you can only call const member functions, which can only call other const functions...

### The const Sandwich Pattern (agentty's architecture)

```
Input (const) → Pure Function → Output (new value)
```

**Example: The update function**
```cpp
// Takes Model by VALUE (owned), returns new Model
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // m is mutable here (we own it)
    return std::visit(overload{
        [&](StreamTextDelta delta) -> std::pair<Model, Cmd<Msg>> {
            m.stream.text += delta.text;  // Mutate owned Model
            return {std::move(m), Cmd<Msg>::none()};
        },
        // ... other handlers
    }, msg);
}
```

**Why this works:**
1. Input is moved into function (no copy)
2. Function mutates its local copy
3. Returns by value (RVO optimization)
4. Caller gets new Model, old one is gone

**Not an OOP mutating method:**
```cpp
// NOT like this (imperative style):
void Model::handle_text_delta(StreamTextDelta delta) {
    this->stream.text += delta.text;  // Mutating shared state
}
```

---

## 1.4 Type Deduction: auto and decltype

### auto: Let the Compiler Figure It Out

```cpp
auto x = 42;                      // int
auto y = 3.14;                    // double
auto s = std::string{"hello"};    // std::string
auto t = make_thread();           // Thread (whatever make_thread returns)
```

**When to use auto:**
1. **Iterator types** — `auto it = vec.begin()` vs `std::vector<T>::iterator it = ...`
2. **Lambda types** — Cannot name them otherwise
3. **Template return types** — Readability

**When NOT to use auto:**
1. **Unclear types** — `auto x = get_something()` — what is x?
2. **Intentional conversions** — `int x = get_double()` truncates, `auto x = get_double()` doesn't

**Real example from agentty:**
```cpp
// Clear: we're iterating over messages
for (const auto& msg : thread.messages) {
    process(msg);
}

// Unclear: what type is step?
auto step = update(model, msg);  // Bad
std::pair<Model, Cmd<Msg>> step = update(model, msg);  // Better
auto [next_model, cmd] = update(model, msg);  // Best (structured binding)
```

### decltype: Get the Type of an Expression

```cpp
int x = 42;
decltype(x) y = 100;  // y is int

const Thread& get_thread();
decltype(get_thread()) t = ...;  // t is const Thread&
```

**Used in templates (advanced topic, Chapter 12):**
```cpp
template <typename F, typename Arg>
auto apply(F f, Arg arg) -> decltype(f(arg)) {
    return f(arg);
}
```

---

## 1.5 Exercises

### Exercise 1.1: Strong Types
Create strong types for a simplified thread system:
```cpp
// TODO: Define ThreadId, MessageId, UserId
// Implement:
// - Explicit constructors from std::string
// - Equality comparison
// - Hash function (for use in unordered_map)

// Test:
ThreadId tid{"thread_001"};
MessageId mid{"msg_001"};
// This should NOT compile:
// process_thread(mid);
```

**Solution:** See `solutions/ch01/ex1-strong-types.cpp`

### Exercise 1.2: const Correctness
Fix the const-correctness issues:
```cpp
class ThreadList {
    std::vector<Thread> threads_;
public:
    // TODO: Make this const-correct
    Thread& get(size_t idx) {
        return threads_.at(idx);
    }
    
    // TODO: Add const overload
    
    // TODO: Make this const
    size_t size() {
        return threads_.size();
    }
};
```

**Solution:** See `solutions/ch01/ex2-const.cpp`

### Exercise 1.3: References vs. Pointers
When should you use references vs. pointers? For each scenario, choose the best option and explain why:

1. Function parameter: large struct you want to read but not modify
2. Function return: might not find the requested object
3. Class member: always present, owned by the class
4. Function parameter: small int you want to modify

**Solution:** See `solutions/ch01/ex3-refs-pointers.md`

### Exercise 1.4: Build a Miniature Type System
Implement a simplified version of agentty's ID system with:
- Three ID types: UserId, SessionId, RequestId
- A Registry that stores objects by ID
- Compile-time safety (wrong ID type = compile error)

**Starter code:** `exercises/ch01/ex4-registry.cpp`  
**Solution:** `solutions/ch01/ex4-registry.cpp`

---

## Key Takeaways

1. **Use types to make illegal states unrepresentable**
   - Strong types catch bugs at compile time
   - Zero runtime cost

2. **Prefer references over pointers for non-nullable parameters**
   - Clearer intent
   - Safer (no null checks)

3. **Use const everywhere possible**
   - Documents immutability
   - Enables optimizations
   - Required for functional architecture

4. **auto for clarity, explicit types for intent**
   - Use auto for obvious types (iterators, lambdas)
   - Use explicit types when conversion matters

---

## Next Chapter

[Chapter 2: Memory Management — RAII and Ownership →](../ch02-memory/README.md)

In the next chapter, you'll learn:
- How C++ manages memory (stack vs heap)
- RAII: the pattern that prevents resource leaks
- Ownership and lifetimes
- The Rule of Zero/Three/Five
