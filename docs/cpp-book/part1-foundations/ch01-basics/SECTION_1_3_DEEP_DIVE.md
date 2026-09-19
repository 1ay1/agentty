# Section 1.3 Deep Dive: const Correctness

## Learning Outcomes
By the end of this section, you will:
1. **Understand what `const` really means** (compile-time guarantee, not runtime)
2. **Apply const-correctness systematically** to your code
3. **Recognize the patterns** that emerge from proper const usage
4. **Understand how const propagates** through codebases
5. **Build const-correct architectures** like agentty's

---

## Part A: What `const` Actually Is

### const is a Compile-Time Promise

```cpp
const int x = 42;
x = 43;  // COMPILE ERROR
```

**What the compiler does:**
1. At compile time: sees `x` is `const`
2. Later: sees assignment to `x`
3. Result: refuses to compile the program

**At runtime:**
- No runtime checks
- No penalty for const variables
- The promise is enforced by the compiler, not by the CPU

### This Is Different from Dynamically-Typed Language "Immutability"

```python
# Python: "immutability" is a suggestion
x = 42
x = 43  # Python allows this, no error
```

```cpp
// C++: const is enforced
const int x = 42;
x = 43;  // Compile error: CANNOT happen
```

---

## Part B: const in Different Contexts

### const Variables

```cpp
const int x = 42;     // x cannot be modified
int const x = 42;     // Same meaning (different order)
x = 43;               // COMPILE ERROR
```

### const Pointers

```cpp
int x = 42;

// Pointer to const int (cannot modify *ptr)
const int* ptr = &x;
*ptr = 43;            // COMPILE ERROR
ptr = nullptr;        // OK: can rebind ptr

// Const pointer to int (cannot rebind ptr)
int* const ptr = &x;
*ptr = 43;            // OK: can modify through ptr
ptr = nullptr;        // COMPILE ERROR: cannot rebind ptr

// Const pointer to const int (cannot modify or rebind)
const int* const ptr = &x;
*ptr = 43;            // COMPILE ERROR
ptr = nullptr;        // COMPILE ERROR
```

**Memory aid:** Read right-to-left:
- `const int*` = pointer to (const int) = can't modify what's pointed to
- `int* const` = const (pointer to int) = can't rebind the pointer
- `const int* const` = const (pointer to const int) = can't do either

### const References (The Most Important)

```cpp
const int x = 42;
const int& ref = x;   // Reference to const int
ref = 43;             // COMPILE ERROR
```

**Key property:** References cannot be rebound anyway, so `const` only adds the "can't modify" guarantee:

```cpp
int x = 42;
const int& ref = x;   // x is not const, but ref can't modify it
ref = 43;             // COMPILE ERROR
x = 43;               // OK: x itself is mutable
```

This is subtle but important: you can have a const reference to a mutable object.

---

## Part C: const Member Functions (The Game Changer)

### The Problem

```cpp
class Thread {
    std::vector<Message> messages_;
    
public:
    std::vector<Message>& messages() {
        return messages_;
    }
};

// This is dangerous:
const Thread& thread = load_thread();
thread.messages().push_back(msg);  // ALLOWED! We're modifying a "const" Thread!
```

**The issue:** We have a const reference to Thread, but we can call a non-const member function, which returns a non-const reference to messages. We just modified a "const" object!

### The Solution: const Member Functions

```cpp
class Thread {
    std::vector<Message> messages_;
    
public:
    // const member function: promise not to modify "this"
    const std::vector<Message>& messages() const {
        return messages_;
    }
};

// Now:
const Thread& thread = load_thread();
thread.messages().push_back(msg);  // COMPILE ERROR: messages() returns const ref
```

**What const member function means:**
```cpp
void some_method() const {
    // In here, "this" is treated as "const Thread*", not "Thread*"
    // Can call other const member functions
    // Cannot call non-const member functions
    // Cannot modify member variables
}
```

### Const and Non-Const Overloads

You can provide both:

```cpp
class Thread {
    std::vector<Message> messages_;
    
public:
    // Read-only version
    const std::vector<Message>& messages() const {
        return messages_;
    }
    
    // Mutable version
    std::vector<Message>& messages_mut() {
        return messages_;
    }
};

// Usage:
const Thread& ct = load_const_thread();
const auto& msgs = ct.messages();  // Calls const version, returns const ref

Thread& mt = load_mut_thread();
auto& msgs = mt.messages_mut();    // Calls non-const version, returns mutable ref
msgs.push_back(new_msg);           // Allowed
```

**Convention in agentty:**
- `obj()` or `obj() const` for read-only access
- `obj_mut()` for mutable access

---

## Part D: How const Propagates

### The Propagation Law

**If you have a const reference to an object, you can ONLY call const member functions on it.**

This creates a natural "const layer" that propagates through your codebase:

```cpp
// Core function: takes const Thread, must only read
void analyze_thread(const Thread& thread) {
    // Can only call const member functions:
    const auto& msgs = thread.messages();
    std::cout << msgs.size();
    
    // CANNOT call non-const members:
    // thread.messages_mut().push_back(msg);  // ERROR
}

// Caller provides const Thread
void process_model(const Model& model) {
    analyze_thread(model.thread());  // Works: thread() is const, returns const ref
}

// Top level: everything is const-correct by construction
void load_and_analyze() {
    const Model& model = load_model();  // Load as const
    process_model(model);               // Propagates const down
}
```

### The Const Chain

When const propagates, it creates a clear data flow:

```
Input (const) → Pure computation → Output (new value)
```

**Example from agentty:**

```cpp
// update: takes owned Model, returns modified Model
// input is NOT const (we own it)
// but we treat it as const initially
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    const Thread& thread = m.thread();  // Borrow as const
    
    return std::visit(overload{
        [&](StreamTextDelta delta) -> std::pair<Model, Cmd<Msg>> {
            m.stream.text += delta.text;  // Mutate our copy
            return {std::move(m), Cmd<Msg>::none()};
        }
    }, msg);
}

// view: takes const Model, produces Element
Element view(const Model& model) {
    const Thread& thread = model.thread();  // Borrow as const
    return render_thread(thread);
}
```

---

## Part E: The Const Correctness Philosophy

### Core Principle: Make Illegal States Unrepresentable

Just like strong types prevent ID confusion, **const correctness prevents accidental mutations**.

```cpp
// WEAK: looks safe, but isn't
class Repository {
    std::vector<Item> items;
    
public:
    void display_all() {
        for (auto& item : items) {  // Not const! Could modify
            std::cout << item;
        }
    }
};

// STRONG: const-correct
class Repository {
    std::vector<Item> items;
    
public:
    void display_all() const {  // Const member function
        for (const auto& item : items) {  // Const reference
            std::cout << item;
        }
    }
};
```

### Properties of Const-Correct Code

1. **Intent is clear from the type signature**
   - `const Thread&` means "I will not modify this"
   - `Thread&` means "I might modify this"

2. **Compiler enforces the promise**
   - Try to modify a const object → compile error
   - No runtime checks, no performance penalty

3. **Refactoring becomes safer**
   - Change one function to const → compiler guides you
   - All callers automatically benefit

4. **Reasoning about state is easier**
   - If a function takes only const refs, it's side-effect-free (from caller's perspective)
   - Data flow is obvious

---

## Part F: Common const Mistakes

### Mistake 1: Forgetting const on Member Functions

```cpp
// BAD: member function claims not to modify, but isn't marked const
class Thread {
    std::vector<Message> messages_;
    
public:
    // Should this be const? Not marked either way
    size_t message_count() {
        return messages_.size();
    }
};

// Consequence:
const Thread& t = load_thread();
t.message_count();  // COMPILE ERROR: non-const member function on const object
```

**Fix:** Mark it const:
```cpp
size_t message_count() const {
    return messages_.size();
}
```

### Mistake 2: Forgetting const on Const Iterators

```cpp
// BAD: iterates with non-const reference
void display_messages(const Thread& thread) {
    const auto& msgs = thread.messages();
    for (auto& msg : msgs) {  // Non-const reference
        std::cout << msg.text;
    }
}

// This is unusual (takes const ref, but uses non-const iter)
// Should be:
void display_messages(const Thread& thread) {
    const auto& msgs = thread.messages();
    for (const auto& msg : msgs) {  // Const reference
        std::cout << msg.text;
    }
}
```

### Mistake 3: Missing const Overloads

```cpp
// BAD: only one version of getter
class Thread {
    std::vector<Message> messages_;
    
public:
    std::vector<Message>& messages() {
        return messages_;
    }
};

// Result:
const Thread& t = const_load_thread();
t.messages();  // ERROR: can't call non-const member on const object

Thread& t = mut_load_thread();
t.messages();  // OK
```

**Fix:** Provide both versions:
```cpp
const std::vector<Message>& messages() const {
    return messages_;
}

std::vector<Message>& messages_mut() {
    return messages_;
}
```

### Mistake 4: const Correctness Doesn't Prevent All Bugs

```cpp
// const-correct, but still has a subtle bug
class ThreadList {
    std::vector<Thread> threads;
    
public:
    const Thread& get(size_t idx) const {
        return threads.at(idx);
    }
    
    void process_all(std::function<void(const Thread&)> handler) const {
        for (const auto& t : threads) {
            handler(t);  // Const is preserved through callback
        }
    }
};

// Usage is safe:
const ThreadList& tl = load_threadlist();
tl.process_all([](const Thread& t) {
    std::cout << t.title;  // Cannot modify t
});
```

This is actually fine. Point: **const-correctness helps, but doesn't eliminate all bugs**. It's a tool, not a panacea.

---

## Part G: Complete Working Example

```cpp
#include <iostream>
#include <string>
#include <vector>
#include <memory>

// ============================================================================
// Domain types
// ============================================================================

class Message {
    std::string id;
    std::string text;
    
public:
    Message(std::string id, std::string text)
        : id(std::move(id)), text(std::move(text)) {}
    
    // Const getter
    const std::string& get_text() const {
        return text;
    }
    
    // Mutable getter
    std::string& text_mut() {
        return text;
    }
    
    // Const member function
    void print() const {
        std::cout << "[" << id << "] " << text << "\n";
    }
};

class Thread {
    std::string id;
    std::vector<Message> messages;
    
public:
    Thread(std::string id) : id(std::move(id)) {}
    
    // Const getter
    const std::vector<Message>& get_messages() const {
        return messages;
    }
    
    // Mutable getter
    std::vector<Message>& messages_mut() {
        return messages;
    }
    
    // Const member function
    void print_all() const {
        std::cout << "Thread " << id << ":\n";
        for (const auto& msg : messages) {
            msg.print();
        }
    }
    
    // Mutating member function
    void add_message(Message msg) {
        messages.push_back(std::move(msg));
    }
};

// ============================================================================
// Functions demonstrating const propagation
// ============================================================================

// Takes const Thread, can only read
void display_thread(const Thread& thread) {
    thread.print_all();
    // thread.add_message(...);  // ERROR: non-const member function
}

// Takes non-const Thread, can read and write
void modify_thread(Thread& thread, Message msg) {
    thread.add_message(std::move(msg));
}

// Returns const reference: borrowed, read-only
const std::vector<Message>& get_messages(const Thread& thread) {
    return thread.get_messages();
}

// Returns value: owned by caller
Thread create_empty_thread() {
    return Thread{"new_thread"};
}

// ============================================================================
// Main: demonstrates const-correct usage
// ============================================================================

int main() {
    // Create and populate
    Thread t{"thread_001"};
    modify_thread(t, Message{"msg_1", "Hello"});
    modify_thread(t, Message{"msg_2", "World"});
    
    // Display const reference (no copy)
    display_thread(t);
    
    // Get const reference to messages (no copy)
    const auto& msgs = get_messages(t);
    std::cout << "Message count: " << msgs.size() << "\n";
    
    // Const object: can only call const member functions
    const Thread& const_t = t;
    const_t.print_all();  // OK: const member function
    // const_t.add_message(...);  // ERROR: non-const member function
    
    // Create new thread: value ownership
    Thread t2 = create_empty_thread();
    modify_thread(t2, Message{"msg_1", "New thread"});
    
    return 0;
}
```

**Output:**
```
Thread thread_001:
[msg_1] Hello
[msg_2] World
Message count: 2
Thread thread_001:
[msg_1] Hello
[msg_2] World
```

---

## Part H: Exercises

### Exercise 1.3.1: Add Const Correctness

**Task:** Fix the const-correctness issues in this code:

```cpp
class Model {
    std::string system_prompt;
    Thread thread;
    
public:
    // TODO: Mark const where appropriate
    std::string get_prompt() {
        return system_prompt;
    }
    
    // TODO: Provide const and mutable versions
    Thread& get_thread() {
        return thread;
    }
    
    // TODO: Mark const
    void print_info() {
        std::cout << "Model: " << system_prompt << "\n";
        std::cout << "Thread: " << thread.id << "\n";
    }
};

// TODO: Mark const where appropriate
void display_model(Model m) {
    m.print_info();
}

// TODO: Mark const where appropriate
void modify_model(Model m, Message msg) {
    m.get_thread().add_message(std::move(msg));
}
```

### Exercise 1.3.2: Const Propagation

**Task:** Trace through this code and identify where const should propagate:

```cpp
class Repository {
    std::vector<Item> items;
    
public:
    const std::vector<Item>& all() const {
        return items;
    }
    
    // TODO: Add const version
    Item& get(size_t idx) {
        return items[idx];
    }
};

// TODO: Mark const appropriately
void display_all(Repository repo) {
    for (const auto& item : repo.all()) {
        std::cout << item << "\n";
    }
}

// TODO: Mark const appropriately
void find_and_display(Repository repo, const std::string& query) {
    // Pseudo-code: find item matching query
    // if (Item* found = find(repo, query)) {
    //     std::cout << *found << "\n";
    // }
}
```

### Exercise 1.3.3: Const and Non-Const Overloads

**Task:** Implement both versions of these getters:

```cpp
class Cache {
    std::vector<std::string> data;
    
public:
    // TODO: Const version
    const std::vector<std::string>& get_data() const { /* ... */ }
    
    // TODO: Mutable version
    std::vector<std::string>& get_data_mut() { /* ... */ }
};

// Usage:
const Cache& c = load_cache();
c.get_data();  // Should call const version, return const ref

Cache& c_mut = load_cache_mut();
c_mut.get_data_mut();  // Should call mutable version, return mutable ref
c_mut.get_data_mut().push_back("new item");  // Allowed
```

---

## Part I: Mastery Quiz

1. **What does `const` mean in C++? Is it runtime or compile-time?**

2. **What's the difference between `const int*` and `int* const`?**

3. **When should a member function be marked `const`?**

4. **If a member function is `const`, what does that mean for `this`?**

5. **Why does const propagate through a codebase?**

6. **Write const-correct versions of these getters:**
```cpp
class Thread {
    std::vector<Message> messages;
    
public:
    // TODO: Const version (read-only)
    std::vector<Message>& messages() { /* ... */ }
    
    // TODO: Mutable version (read-write)
};
```

---

## Key Takeaways for Section 1.3

1. **const is a compile-time promise** — no runtime cost, compiler enforces it
2. **Mark member functions const** when they don't modify the object
3. **Provide both const and mutable overloads** for getters that return references
4. **const propagates naturally** through your codebase
5. **Const-correct code is self-documenting** — types reveal intent

---

## Next: Section 1.4

Once you've mastered Section 1.3, you're ready for **Type Deduction**, where `auto` and `decltype` interact with the type system you've just learned.
