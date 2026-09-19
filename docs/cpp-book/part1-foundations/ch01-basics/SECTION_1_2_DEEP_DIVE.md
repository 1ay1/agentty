# Section 1.2 Deep Dive: Values, References, and Pointers

## Learning Outcomes
By the end of this section, you will:
1. **Understand the three ways to refer to data** and how they differ at the CPU level
2. **Know exactly when to use each** (values, const refs, mutable refs, pointers)
3. **Build intuition** about what happens in memory during copies, moves, and passing
4. **Explain the lifetime rules** that make C++ safer than it seems
5. **Apply these patterns** to real agentty code with confidence

---

## Part A: The Three Models Explained (At the CPU Level)

### Values: Ownership and Independence

```cpp
std::string text = "hello";  // Value: heap allocation on this line
std::string copy = text;     // Value: NEW heap allocation
copy[0] = 'H';               // Mutates copy's heap, not text's
std::cout << text;           // Prints "hello" (unchanged)
std::cout << copy;           // Prints "Hello"
```

**What happens in memory:**
```
Before assignment:
text: String object
      ├── ptr → heap: [h,e,l,l,o,\0]
      └── size: 5, capacity: 5

copy = text;
(Copy constructor runs)

After assignment:
text: String object (ORIGINAL)
      ├── ptr → heap: [h,e,l,l,o,\0]  (ORIGINAL memory)
      └── size: 5, capacity: 5

copy: String object (NEW)
      ├── ptr → heap: [h,e,l,l,o,\0]  (NEW memory, NEW allocation)
      └── size: 5, capacity: 5
```

**Key properties of values:**
1. **Each value owns its own data** — complete independence
2. **Copying creates a new allocation** — expensive for large objects
3. **Cannot be null** — it always exists
4. **Lifetime is determined by scope** — goes away when it goes out of scope

**When to use:**
- You need independent copies (each mutation should not affect others)
- Return values from functions (automatic optimization)
- Data is small (< 64 bytes)
- You're moving, not copying (see Chapter 4)

### References: Aliasing Without Ownership

```cpp
std::string text = "hello";
std::string& ref = text;     // Reference: alias for text
ref[0] = 'H';                // Mutates text through ref
std::cout << text;           // Prints "Hello"
```

**What happens in memory:**
```
text: String object
      ├── ptr → heap: [H,e,l,l,o,\0]
      └── size: 5, capacity: 5

ref: (Compiled away to a pointer, but semantically "the same object as text")
```

**At the CPU level:** A reference compiles to a pointer, but with compile-time guarantees:
- Cannot be null
- Cannot be rebound to a different object
- The compiler enforces this

**Under the hood (C-ish pseudocode):**
```cpp
// Your C++ code:
std::string text = "hello";
std::string& ref = text;
ref[0] = 'H';

// Compiled code (conceptually):
std::string text = ...;
std::string* ref = &text;  // Implicit &
(*ref)[0] = 'H';           // Implicit *
```

**But the compiler adds safety:**
- If you try to bind ref to a temporary, compiler errors
- If you try to rebind ref, compiler errors
- If you try to null-check a reference, compiler errors

**Key properties of references:**
1. **Cannot be null** — compilation enforces this
2. **Cannot be rebound** — once bound to an object, always refers to it
3. **Zero runtime overhead** — compiled to pointer, but no extra checks
4. **Lifetime tied to referenced object** — if object is destroyed, reference becomes dangling (but compiler helps you avoid this)

**When to use:**
- Avoid copies of large objects
- Modify function arguments
- Return subobjects from a container
- Function parameters where null is meaningless

### Pointers: The Flexible Alternative

```cpp
std::string* ptr = nullptr;       // Can be null
std::string text = "hello";
ptr = &text;                      // Can point to an object
std::cout << *ptr;                // Dereference to access

ptr = nullptr;                    // Can be set to null
if (ptr != nullptr) {
    std::cout << *ptr;            // Safe to dereference only if not null
}
```

**What happens in memory:**
```
ptr: Variable holding a memory address (8 bytes on 64-bit)
     value: (null, or points to text)

text: String object at some memory location
      ├── ptr → heap: [h,e,l,l,o,\0]
      └── size: 5, capacity: 5
```

**Key properties of pointers:**
1. **Can be null** — you can check `if (ptr != nullptr)`
2. **Can be rebound** — `ptr = &other_object` is allowed
3. **Manual dereferencing** — must use `*ptr` to access
4. **Explicit ownership unclear** — does this pointer own the object?

**When to use:**
- Nullable parameters (optional references)
- Polymorphism (base class pointers to derived objects)
- Dynamic memory (but prefer `std::unique_ptr`, Chapter 5)
- C APIs that require pointers

**When NOT to use (in modern C++):**
- Raw pointers for ownership (use `std::unique_ptr` instead)
- Multiple pointers to same object (use `std::shared_ptr` instead)
- Null checks everywhere (use `std::optional` instead, Chapter 2)

---

## Part B: Deciding Between References, Pointers, and Values

### Decision Tree

**Is the object optional (might not exist)?**
- Yes → Use `std::optional<T>` or `T*` (pointer)
- No → Go to next question

**Is the object large (> 64 bytes)?**
- Yes → Use reference or move (Chapter 4)
- No → Go to next question

**Does the function modify the object?**
- Yes → Use non-const reference `T&`
- No → Use const reference `const T&`

**Exception: Return values**
- Always use value (automatic RVO optimization, Chapter 4)

### Real Examples from agentty

#### Example 1: Reading Large Objects (const ref)

```cpp
// BAD: Copies the entire Message struct
void render_message(Message msg) {
    std::cout << msg.text;
    for (const auto& img : msg.images) {
        render_image(img);
    }
}

// GOOD: Borrows reference, zero copy
void render_message(const Message& msg) {
    std::cout << msg.text;
    for (const auto& img : msg.images) {
        render_image(img);
    }
}

// Usage:
const auto& msg = thread.messages[0];
render_message(msg);  // No copy happens
```

#### Example 2: Modifying Objects (mutable ref)

```cpp
// Function that modifies a Thread
void add_message(Thread& thread, Message msg) {
    // Takes non-const reference: signals "I will modify this"
    thread.messages.push_back(std::move(msg));
    thread.updated_at = now();
}

// Usage:
Thread t = load_thread();
add_message(t, msg);  // t is modified
std::cout << t.updated_at;  // Reflects the update
```

#### Example 3: Optional Results (pointer or optional)

```cpp
// Old style (pointer):
const std::string* get_tool_output(const ToolUse& tool) {
    if (auto* d = std::get_if<Done>(&tool.state)) {
        return &d->output;
    }
    return nullptr;
}

// Usage:
if (const auto* output = get_tool_output(tool)) {
    process(*output);
}

// Modern style (std::optional, Chapter 2):
std::optional<std::string_view> get_tool_output(const ToolUse& tool) {
    if (auto* d = std::get_if<Done>(&tool.state)) {
        return std::string_view{d->output};
    }
    return std::nullopt;
}

// Usage:
if (auto output = get_tool_output(tool)) {
    process(*output);
}
```

#### Example 4: Returning Subobjects (const ref)

```cpp
class Thread {
    std::vector<Message> messages_;
    
public:
    // Return const reference to prevent accidental modification
    const std::vector<Message>& messages() const {
        return messages_;
    }
    
    // Mutable version for when we need to modify
    std::vector<Message>& messages_mut() {
        return messages_;
    }
};

// Usage:
const Thread& t = load_thread();
for (const auto& msg : t.messages()) {
    // t.messages() returns a const reference
    // No copy of the entire vector
    process(msg);
}
```

---

## Part C: The Lifetime Problem (And How References Help)

### The Dangling Reference Problem

A **dangling reference** is a reference to an object that no longer exists:

```cpp
std::string& get_dangerous() {
    std::string local = "hello";
    return local;  // ERROR: returning reference to local variable
    // local is destroyed when function exits
    // returning reference to dead memory
}

// Bad code:
std::string& ref = get_dangerous();  // ref points to destroyed object
std::cout << ref;                    // UNDEFINED BEHAVIOR
```

**The compiler catches this:**
```
error: returning reference to local variable 'local' [-Werror,-Wreturn-stack-address]
```

### Why Lifetimes Matter

C++ has automatic memory management through RAII (Chapter 2), but you must follow the rules:

```cpp
// SAFE: Owner destroys when out of scope
std::string* get_safe() {
    static std::string value = "hello";  // Static: lives for program duration
    return &value;
}

std::string* ptr = get_safe();
std::cout << *ptr;  // Safe: value still exists
```

```cpp
// ALSO SAFE: Returning by value
std::string get_safe() {
    std::string value = "hello";
    return value;  // Copy/move returned, caller owns it
}

std::string s = get_safe();  // Caller now owns the string
std::cout << s;              // Safe
```

### Lifetime Rules (The Core Concept)

**Rule 1: Values own their data**
```cpp
std::string s = "hello";  // s owns the memory
// When s is destroyed, memory is freed
```

**Rule 2: References don't own; they just alias**
```cpp
std::string s = "hello";
std::string& ref = s;      // ref does NOT own s's memory
// When ref goes out of scope, s's memory is NOT freed
```

**Rule 3: You can't return references to local variables**
```cpp
std::string& bad() {
    std::string local = "hello";
    return local;  // Compiler prevents this
}
```

**Rule 4: References to function parameters are safe**
```cpp
std::string& get_param(std::string& s) {
    return s;  // OK: s outlives this function
}

std::string value = "hello";
std::string& ref = get_param(value);  // Safe: value still exists
```

**Rule 5: const refs to temporaries are allowed (and safe)**
```cpp
const std::string& ref = std::string{"hello"};  // OK!
// Compiler extends the lifetime of temporary to match ref
std::cout << ref;  // Safe, temporary still exists
```

### Visualizing Lifetimes

```cpp
{
    std::string s = "hello";    // s created here
    
    {
        std::string& ref = s;   // ref aliases s (not owned)
        std::cout << ref;       // Safe: s exists
    }                           // ref destroyed (no harm, s still exists)
    
    std::cout << s;             // Safe: s still exists
}                               // s destroyed, memory freed
```

---

## Part D: Copy vs Move (Preview to Chapter 4)

For now, you need to know:

### Copying (Expensive)

```cpp
std::string text = "hello";
std::string copy = text;  // Allocates new memory, copies data
                          // Now two independent strings exist
```

### Moving (Cheap)

```cpp
std::string text = "hello";
std::string moved = std::move(text);  // Transfers ownership
                                      // text is now empty
                                      // moved owns the memory
                                      // No new allocation
```

**The rule:** If you're done with an object, use `std::move()` to transfer ownership cheaply.

```cpp
// AVOID: Unnecessary copy
void process(std::string text) {
    store_string(text);  // text is copied into storage
    // text is then destroyed
}

// BETTER: Move instead
void process(std::string text) {
    store_string(std::move(text));  // text is moved (cheap)
    // text is now empty, but we don't care
}

// BEST: Take by rvalue reference
void process(std::string&& text) {
    store_string(std::move(text));  // Move (cheap)
}
```

See Chapter 4 for full details.

---

## Part E: Common Mistakes and How to Avoid Them

### Mistake 1: Copying When You Should Use References

```cpp
// BAD: Thread is copied (allocation for all messages, all streams, etc.)
void handle_message(Thread thread, Message msg) {
    thread.messages.push_back(std::move(msg));
}

// GOOD: Thread is borrowed
void handle_message(Thread& thread, Message msg) {
    thread.messages.push_back(std::move(msg));
}

// GOOD: Thread is read-only
void display_messages(const Thread& thread) {
    for (const auto& msg : thread.messages) {
        std::cout << msg.text << "\n";
    }
}
```

### Mistake 2: Using Pointers When References Would Be Clearer

```cpp
// BAD: Pointer allows null, but we require non-null
void process(Model* model) {
    if (model == nullptr) {
        throw std::runtime_error("model required");
    }
    // Now use model...
}

// GOOD: Reference enforces non-null at compile time
void process(const Model& model) {
    // No null check needed, compiler guarantees non-null
}
```

### Mistake 3: Returning References to Local Variables (Compiler Prevents This)

```cpp
// COMPILER ERROR: cannot return reference to local
const std::string& get_string() {
    std::string local = "hello";
    return local;  // ERROR: -Wreturn-stack-address
}

// CORRECT: Return by value
std::string get_string() {
    std::string local = "hello";
    return local;  // Caller owns the returned copy
}
```

### Mistake 4: Holding Dangling Pointers

```cpp
// BAD:
std::string* ptr = nullptr;
{
    std::string s = "hello";
    ptr = &s;
    std::cout << *ptr;  // OK here
}
// s is destroyed, ptr is now dangling
std::cout << *ptr;      // UNDEFINED BEHAVIOR

// GOOD: Keep objects alive
std::string s = "hello";
{
    std::string* ptr = &s;
    std::cout << *ptr;  // OK
}
std::cout << s;         // OK, s still alive
```

---

## Part F: Complete Working Examples

### Example 1: Immutable Object (Read-Only)

```cpp
class Thread {
    ThreadId id_;
    std::vector<Message> messages_;
    
public:
    // Const member function: can only call from const Thread
    const ThreadId& id() const { return id_; }
    
    // Returns const reference to container
    const std::vector<Message>& messages() const {
        return messages_;
    }
};

// Usage:
const Thread& thread = load_thread();
const ThreadId& tid = thread.id();           // Const reference, no copy
const auto& msgs = thread.messages();         // Const reference, no copy
for (const auto& msg : msgs) {
    std::cout << msg.text << "\n";           // No copies here either
}
```

### Example 2: Mutable Object (Read-Write)

```cpp
class Thread {
    ThreadId id_;
    std::vector<Message> messages_;
    
public:
    const ThreadId& id() const { return id_; }
    
    const std::vector<Message>& messages() const {
        return messages_;
    }
    
    // Mutable version returns non-const reference
    std::vector<Message>& messages_mut() {
        return messages_;
    }
};

// Usage:
Thread thread = load_thread();
thread.messages_mut().push_back(new_message);  // Modifies thread
```

### Example 3: Function Showing All Three Types

```cpp
#include <iostream>
#include <string>
#include <vector>

class Repository {
    std::vector<std::string> items;
    
public:
    // Add item by value (or move)
    void add(std::string item) {
        items.push_back(std::move(item));
    }
    
    // Read one item by const reference (no copy)
    const std::string& get(size_t idx) const {
        return items.at(idx);
    }
    
    // Modify one item by mutable reference
    std::string& get_mut(size_t idx) {
        return items.at(idx);
    }
    
    // Search returns pointer (nullable) or optional
    const std::string* find(const std::string& query) const {
        for (const auto& item : items) {
            if (item.find(query) != std::string::npos) {
                return &item;  // Found: return pointer to it
            }
        }
        return nullptr;  // Not found: return null
    }
    
    // Read all items const reference (no copy)
    const std::vector<std::string>& all() const {
        return items;
    }
};

int main() {
    Repository repo;
    
    // Add items by value/move
    repo.add("first");
    repo.add(std::string{"second"});
    
    // Read by const reference
    const auto& item0 = repo.get(0);
    std::cout << "Item 0: " << item0 << "\n";
    
    // Modify by mutable reference
    repo.get_mut(0) += " (modified)";
    std::cout << "Item 0: " << repo.get(0) << "\n";
    
    // Find returns pointer (nullable)
    if (const auto* found = repo.find("ond")) {
        std::cout << "Found: " << *found << "\n";
    } else {
        std::cout << "Not found\n";
    }
    
    // Iterate using const reference
    std::cout << "All items:\n";
    for (const auto& item : repo.all()) {
        std::cout << "  - " << item << "\n";
    }
    
    return 0;
}
```

**Output:**
```
Item 0: first
Item 0: first (modified)
Found: second
All items:
  - first (modified)
  - second
```

---

## Part G: Exercises

### Exercise 1.2.1: Redesign with References

**Task:** Take this code and fix it to use references instead of copies:

```cpp
// CURRENT (inefficient):
class ChatSession {
    std::vector<Thread> threads;
    
public:
    void add_thread(Thread t) {
        threads.push_back(t);  // Copies entire Thread
    }
    
    void display_thread(Thread t) {
        std::cout << t.title << ": " << t.messages.size() << " messages\n";
    }
    
    Thread get_thread(size_t idx) {
        return threads[idx];  // Copies entire Thread
    }
};

// TODO: Refactor to use:
// - Const references for read-only access
// - Mutable references for modification
// - Values only for return values (ownership transfer)
```

### Exercise 1.2.2: Implementing Getters

**Task:** Implement const and mutable getters:

```cpp
class Model {
    std::string system_prompt;
    std::vector<Thread> threads;
    
public:
    // TODO: Implement const version (read-only)
    const std::string& system_prompt() const { /* ... */ }
    
    // TODO: Implement mutable version
    std::string& system_prompt_mut() { /* ... */ }
    
    // TODO: Const version for threads
    const std::vector<Thread>& threads() const { /* ... */ }
    
    // TODO: Mutable version for threads
    std::vector<Thread>& threads_mut() { /* ... */ }
};
```

### Exercise 1.2.3: When to Use Pointer vs Reference vs Value

For each scenario, decide which you'd use and explain why:

1. A function that reads a large struct without modifying it
2. A function return value that might not exist (optional)
3. A class member that is always present and owned by the class
4. A function parameter that modifies its argument
5. Returning a sub-object from a container

---

## Part H: Mastery Quiz

1. **What is the runtime representation of a reference in C++?**

2. **Why can't a reference be null?**

3. **When is it safe to return a reference from a function?**

4. **Explain the difference between `std::string s` and `const std::string& s` in terms of memory and performance.**

5. **When should you use a pointer instead of a reference?**

6. **What makes this code unsafe?**
```cpp
const std::string& get_string() {
    std::string local = "hello";
    return local;
}
```

---

## Key Takeaways for Section 1.2

1. **Values own their data** — independent copies, expensive for large objects
2. **Const references borrow data** — zero copy, read-only, preferred for parameters
3. **Mutable references borrow data** — zero copy, read-write
4. **Pointers are for nullable/polymorphic cases** — manual dereferencing required
5. **Lifetimes must match** — compiler enforces this for references
6. **Use the right type for the right job** — values for ownership, const refs for borrowing

---

## Next: Section 1.3

Once you've mastered Section 1.2, you're ready for **const Correctness**, which applies these concepts systematically.
