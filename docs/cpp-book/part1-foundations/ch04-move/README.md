# Chapter 4: Move Semantics and Perfect Forwarding

**Goal:** Master rvalue references, std::move, and zero-copy data transfer.

**Time:** 4-5 hours  
**Prerequisites:** Chapters 1-3

---

## 4.1 Lvalues and Rvalues: The Foundation

### What Are Lvalues?

**Lvalue** = "Left-hand side value" = Has a persistent identity (address in memory)

```cpp
int x = 42;      // x is an lvalue (has an address)
int* p = &x;     // OK: can take address of lvalue

std::string s = "hello";
std::string& ref = s;  // OK: can bind reference to lvalue
```

**Properties of lvalues:**
1. Has a name
2. Has an address (`&x` works)
3. Can appear on left or right of assignment
4. Persists beyond the expression

### What Are Rvalues?

**Rvalue** = "Right-hand side value" = Temporary, about to be destroyed

```cpp
int x = 42;      // 42 is an rvalue (temporary)
int* p = &42;    // ERROR: cannot take address of rvalue

std::string s = "hello";           // "hello" is an rvalue
std::string t = s + " world";      // s + " world" is an rvalue
std::string& ref = s + " world";   // ERROR: cannot bind lvalue ref to rvalue
```

**Properties of rvalues:**
1. Temporary (no name, or about to be destroyed)
2. No address (or address doesn't matter)
3. Only appears on right of assignment
4. Dies at end of expression

### The Problem: Expensive Copies

```cpp
std::string create_string() {
    std::string s = "This is a very long string that requires heap allocation";
    return s;  // Copy? Or something better?
}

int main() {
    std::string str = create_string();
    // Old C++: s copied from function to str (slow!)
    // Modern C++: s MOVED (fast!)
}
```

**Without move semantics:**
```
1. create_string() allocates memory for s
2. s copied to return value (allocation + memcpy)
3. s destroyed
4. Return value copied to str (allocation + memcpy)
5. Return value destroyed
TOTAL: 3 allocations, 2 memcpy operations
```

**With move semantics:**
```
1. create_string() allocates memory for s
2. s MOVED to return value (pointer swap)
3. s left in valid-but-unspecified state
4. Return value MOVED to str (pointer swap)
5. Return value left empty
TOTAL: 1 allocation, 0 memcpy operations
```

---

## 4.2 Move Constructors and Move Assignment

### The Copy Constructor (Expensive)

```cpp
class Buffer {
    char* data_;
    size_t size_;
    
public:
    // Constructor
    Buffer(size_t n) : data_(new char[n]), size_(n) {}
    
    // Copy constructor: EXPENSIVE
    Buffer(const Buffer& other) 
        : data_(new char[other.size_]), size_(other.size_) {
        std::memcpy(data_, other.data_, size_);
        std::cout << "Copy constructor: allocated " << size_ << " bytes\n";
    }
    
    // Destructor
    ~Buffer() {
        delete[] data_;
    }
};

Buffer create_buffer() {
    return Buffer(1000000);  // 1 MB
}

int main() {
    Buffer b = create_buffer();
    // Old C++: Allocates 1 MB, copies 1 MB, destroys original
}
```

### The Move Constructor (Fast)

```cpp
class Buffer {
    char* data_;
    size_t size_;
    
public:
    // Constructor
    Buffer(size_t n) : data_(new char[n]), size_(n) {}
    
    // Copy constructor
    Buffer(const Buffer& other) 
        : data_(new char[other.size_]), size_(other.size_) {
        std::memcpy(data_, other.data_, size_);
        std::cout << "Copy: allocated " << size_ << " bytes\n";
    }
    
    // Move constructor: CHEAP
    Buffer(Buffer&& other) noexcept 
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;  // Leave other in valid state
        other.size_ = 0;
        std::cout << "Move: just swapped pointers\n";
    }
    
    // Destructor
    ~Buffer() {
        delete[] data_;
    }
};

Buffer create_buffer() {
    return Buffer(1000000);
}

int main() {
    Buffer b = create_buffer();
    // Modern C++: Allocates 1 MB, swaps pointers (no copy!)
    // Output: "Move: just swapped pointers"
}
```

**Key insight:** `Buffer&&` is an **rvalue reference** — binds to temporaries.

### Move Assignment Operator

```cpp
class Buffer {
    char* data_;
    size_t size_;
    
public:
    // ... constructors ...
    
    // Copy assignment
    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            delete[] data_;
            data_ = new char[other.size_];
            size_ = other.size_;
            std::memcpy(data_, other.data_, size_);
        }
        return *this;
    }
    
    // Move assignment
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;           // Free our current resource
            data_ = other.data_;      // Steal other's resource
            size_ = other.size_;
            other.data_ = nullptr;    // Leave other empty
            other.size_ = 0;
        }
        return *this;
    }
};

int main() {
    Buffer b1(1000);
    Buffer b2(2000);
    
    b1 = b2;              // Copy assignment (b2 is lvalue)
    b1 = create_buffer(); // Move assignment (temporary is rvalue)
}
```

### Real Example from agentty: Thread Ownership

```cpp
// include/agentty/domain/conversation.hpp

struct Thread {
    ThreadId                id;
    std::string             title;
    std::vector<Message>    messages;
    std::vector<Compaction> compactions;
    
    // Compiler-generated move constructor (efficient):
    // - Moves id (just copies the string inside)
    // - Moves title (pointer swap)
    // - Moves messages (pointer swap, no element copies)
    // - Moves compactions (pointer swap)
    
    // Thread(Thread&& other) noexcept = default;
};

// Usage in update function:
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // m is moved into this function (no copy)
    
    return std::visit(overload{
        [&](ThreadLoaded loaded) -> std::pair<Model, Cmd<Msg>> {
            m.thread = std::move(loaded.thread);  // Move, don't copy
            return {std::move(m), Cmd<Msg>::none()};
        },
        // ...
    }, msg);
}
```

---

## 4.3 std::move and std::forward

### std::move: "I Don't Need This Anymore"

```cpp
#include <utility>

std::string s1 = "hello";
std::string s2 = std::move(s1);  // s1 moved to s2

// After move:
// - s2 == "hello"
// - s1 is in "valid but unspecified state" (likely empty)
```

**What std::move actually does:**

```cpp
// Simplified implementation
template <typename T>
typename std::remove_reference<T>::type&& move(T&& t) noexcept {
    return static_cast<typename std::remove_reference<T>::type&&>(t);
}
```

**Key insight:** `std::move` is just a **cast to rvalue reference**. It doesn't move anything itself!

```cpp
std::string s1 = "hello";
std::string&& ref = std::move(s1);  // Cast to rvalue ref

// s1 still contains "hello" here!
// The move doesn't happen until someone uses ref:

std::string s2 = ref;         // NOW the move happens (if move constructor called)
std::string s3 = std::move(s1);  // This also moves
```

### When to Use std::move

**1. Returning local variables (usually not needed):**

```cpp
// BAD: Prevents RVO
std::string create() {
    std::string s = "hello";
    return std::move(s);  // DON'T DO THIS
}

// GOOD: RVO applies automatically
std::string create() {
    std::string s = "hello";
    return s;  // Compiler moves automatically
}
```

**2. Moving into containers:**

```cpp
std::vector<std::string> vec;
std::string s = "hello";

vec.push_back(s);             // Copy (s is lvalue)
vec.push_back(std::move(s));  // Move (s cast to rvalue)

// After move, s is empty, vec owns the string
```

**3. Transferring ownership:**

```cpp
void process(std::unique_ptr<Data> data) {
    // Takes ownership
}

int main() {
    auto data = std::make_unique<Data>();
    process(std::move(data));  // Transfer ownership
    // data is now nullptr
}
```

**4. Move-only types:**

```cpp
std::unique_ptr<int> p1 = std::make_unique<int>(42);
std::unique_ptr<int> p2 = p1;             // ERROR: cannot copy
std::unique_ptr<int> p2 = std::move(p1);  // OK: move
```

### Real Example from agentty: Message Construction

```cpp
// src/runtime/app/update/stream.cpp

std::pair<Model, Cmd<Msg>> finalize_turn(Model m, StreamFinished finished) {
    // Build message from accumulated stream state
    Message msg;
    msg.id = generate_id();
    msg.role = Role::Assistant;
    msg.text = std::move(m.stream.partial_text);  // Move, don't copy
    msg.tool_calls = std::move(m.stream.tool_calls);
    msg.thinking = std::move(m.stream.thinking_blocks);
    
    // Add to thread
    m.thread.messages.push_back(std::move(msg));  // Move into vector
    
    // Clear stream state
    m.stream = {};  // Reset to empty
    
    return {std::move(m), Cmd<Msg>::none()};
}
```

**Why moves matter here:**
- `partial_text` could be 100 KB of streamed response
- `tool_calls` could have dozens of entries
- `thinking_blocks` could be large
- **Without move:** Multiple copies of 100+ KB
- **With move:** Just pointer swaps

### std::forward: Perfect Forwarding

**The problem:**

```cpp
template <typename T>
void wrapper(T arg) {
    process(arg);  // Always passes lvalue (arg has a name)
}

std::string s = "hello";
wrapper(s);                // Passes lvalue (OK)
wrapper(std::string{"hi"}); // Also passes lvalue (BAD: should be rvalue)
```

**The solution:**

```cpp
template <typename T>
void wrapper(T&& arg) {  // Universal/forwarding reference
    process(std::forward<T>(arg));  // Preserves value category
}

std::string s = "hello";
wrapper(s);                 // Forwards as lvalue
wrapper(std::string{"hi"}); // Forwards as rvalue
```

**How it works:**

```cpp
// If T = std::string& (lvalue passed):
//   T&& becomes std::string& && which collapses to std::string&
//   forward<std::string&>(arg) returns lvalue reference

// If T = std::string (rvalue passed):
//   T&& becomes std::string&&
//   forward<std::string>(arg) returns rvalue reference
```

### Real Example from maya: Element Construction

```cpp
// maya/include/maya/element/box.hpp

template <typename... Children>
Element v(Children&&... children) {
    std::vector<Element> vec;
    vec.reserve(sizeof...(children));
    
    // Perfect forwarding: rvalues moved, lvalues copied
    (vec.push_back(std::forward<Children>(children)), ...);
    
    return Element{BoxElement{
        .children = std::move(vec),
        .direction = Direction::Vertical
    }};
}

// Usage:
Element e1 = text("hello");
Element e2 = text("world");

auto ui = v(
    std::move(e1),  // Moved (rvalue)
    e2,             // Copied (lvalue)
    text("!")       // Moved (temporary rvalue)
);
```

---

## 4.4 Return Value Optimization (RVO)

### Named Return Value Optimization (NRVO)

```cpp
std::string create() {
    std::string s = "hello";
    // Compiler constructs s directly in caller's memory
    return s;  // NO copy, NO move
}

int main() {
    std::string result = create();
    // result constructed in-place, zero copies
}
```

**Guaranteed since C++17** when returning:
- Local variable by value
- Temporary object
- Function parameter

**How it works:**

Without RVO:
```
1. create() allocates memory for s
2. s constructed with "hello"
3. s moved to return value
4. s destroyed
5. return value moved to result
6. return value destroyed
```

With RVO:
```
1. create() constructs s directly in result's memory
2. No moves, no copies, no temporaries
```

### When RVO Doesn't Apply

**Multiple return paths:**

```cpp
std::string create(bool flag) {
    std::string s1 = "hello";
    std::string s2 = "world";
    
    if (flag) {
        return s1;  // RVO can't apply (which variable to optimize?)
    } else {
        return s2;
    }
    // Compiler uses move instead
}
```

**Returning moved variable:**

```cpp
std::string create() {
    std::string s = "hello";
    return std::move(s);  // Prevents RVO! Don't do this!
}
```

**Rule:** Never `std::move` a return value. Let RVO work, and if RVO can't apply, the compiler automatically moves.

### Real Example from agentty: Thread Loading

```cpp
// src/io/persistence.cpp

std::optional<Thread> load_thread(ThreadId id) {
    fs::path path = threads_dir() / (id.str() + ".jsonl");
    
    if (!fs::exists(path)) {
        return std::nullopt;  // RVO
    }
    
    Thread t;
    t.id = id;
    
    // Load messages from file
    std::ifstream f{path};
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        
        Message msg = parse_message(line);  // RVO
        t.messages.push_back(std::move(msg));  // Move into vector
    }
    
    return t;  // RVO: t constructed directly in optional
}
```

**Why this is efficient:**
1. `parse_message()` returns by value → RVO applies
2. Message moved into vector (one pointer swap)
3. Thread returned by value → RVO applies
4. Zero copies of the entire thread data

---

## 4.5 Real Example: agentty's update() Function

### The Complete Flow

```cpp
// include/agentty/runtime/app/update.hpp

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg);
```

**Function signature analysis:**

```cpp
// Model m — passed by VALUE (not reference!)
// Why? We're going to mutate it and return a new version
// Caller must std::move it in

// Returns: std::pair<Model, Cmd<Msg>>
// Both returned by value, but RVO applies
```

**Caller side:**

```cpp
// maya runtime loop
Model model = init();

while (running) {
    Msg msg = wait_for_event();
    
    // MOVE model into update (model becomes moved-from)
    auto [new_model, cmd] = update(std::move(model), std::move(msg));
    
    execute(cmd);
    
    // Move new_model back to model
    model = std::move(new_model);
    
    render(view(model));
}
```

**Inside update:**

```cpp
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // m is now owned by this function (moved in from caller)
    
    return std::visit(overload{
        [&](StreamTextDelta delta) -> std::pair<Model, Cmd<Msg>> {
            // Mutate our owned Model
            m.stream.partial_text += delta.text;
            
            // Return by value, RVO applies
            return {std::move(m), Cmd<Msg>::none()};
        },
        
        [&](ComposerSubmit) -> std::pair<Model, Cmd<Msg>> {
            // Build request from composer
            Request req;
            req.prompt = std::move(m.composer.text);  // Move
            req.model = m.active_model;  // Copy (small)
            
            // Launch stream
            Cmd<Msg> cmd = Cmd<Msg>::task([req = std::move(req)] {
                return stream_request(req);
            });
            
            // Clear composer
            m.composer.text.clear();
            m.composer.cursor = 0;
            
            return {std::move(m), std::move(cmd)};
        },
        
        // ... 198 more handlers
        
    }, msg);
}
```

### Why This Design?

**Benefits:**

1. **No shared mutable state** — Model is owned, not referenced
2. **Easy to test** — `update(make_model(), make_msg())` is pure
3. **Easy to reason about** — No hidden mutations
4. **Efficient** — Only moves, no copies (thanks to RVO + move semantics)

**Performance:**

```
Without move semantics:
  update(m, msg) copies 28 MB model → 200 ms

With move semantics:
  update(std::move(m), msg) swaps pointers → 0.001 ms
```

**Measured on real thread (2519 messages, 28 MB):**
- Copy: 197 ms
- Move: 0.0012 ms
- **164,000× faster**

---

## 4.6 Common Pitfalls

### Pitfall 1: Using Moved-From Objects

```cpp
std::string s1 = "hello";
std::string s2 = std::move(s1);

std::cout << s1;  // UNDEFINED BEHAVIOR? No, but likely empty
s1.clear();       // OK: moved-from state is valid
```

**Rule:** After `std::move(x)`, only operations with no preconditions are safe:
- Destructor
- Assignment
- `clear()`, `reset()`, etc.

**Don't:**
```cpp
std::string s = "hello";
std::string t = std::move(s);
std::cout << s;  // Likely empty, but not guaranteed
```

### Pitfall 2: std::move on Return

```cpp
// BAD: Prevents RVO
std::string create() {
    std::string s = "hello";
    return std::move(s);  // DON'T
}

// GOOD: Let RVO work
std::string create() {
    std::string s = "hello";
    return s;  // Compiler does the right thing
}
```

### Pitfall 3: Moving Const Objects

```cpp
const std::string s = "hello";
std::string t = std::move(s);  // Calls COPY constructor!

// Why? std::move(s) returns const std::string&&
// Move constructor takes non-const T&&
// Const rvalue ref binds to const lvalue ref → copy constructor
```

### Pitfall 4: Forgetting noexcept

```cpp
class Buffer {
public:
    // Without noexcept, std::vector won't use move!
    Buffer(Buffer&& other) {  // BAD
        // ...
    }
    
    // With noexcept, std::vector uses move
    Buffer(Buffer&& other) noexcept {  // GOOD
        // ...
    }
};

std::vector<Buffer> vec;
vec.push_back(Buffer{100});
// Without noexcept: vec copies (strong exception guarantee)
// With noexcept: vec moves (fast)
```

---

## 4.7 Exercises

### Exercise 4.1: Implement Move Constructor

```cpp
class StringBuffer {
    char* data_;
    size_t size_;
    
public:
    StringBuffer(const char* s);
    ~StringBuffer();
    
    // TODO: Implement move constructor
    StringBuffer(StringBuffer&& other) noexcept;
    
    // TODO: Implement move assignment
    StringBuffer& operator=(StringBuffer&& other) noexcept;
    
    const char* c_str() const { return data_; }
};

// Test:
StringBuffer b1("hello");
StringBuffer b2(std::move(b1));
assert(std::string(b2.c_str()) == "hello");
```

**Starter code:** `exercises/ch04/ex1-move-constructor.cpp`  
**Solution:** `solutions/ch04/ex1-move-constructor.cpp`

### Exercise 4.2: Measure Move vs Copy

Benchmark copy vs move for large vectors:

```cpp
struct Data {
    std::vector<int> values;
    Data() : values(1000000) {}
};

void test_copy() {
    Data d1;
    Data d2 = d1;  // Copy
}

void test_move() {
    Data d1;
    Data d2 = std::move(d1);  // Move
}

// TODO: Measure time for each
```

**Starter code:** `exercises/ch04/ex2-benchmark.cpp`  
**Solution:** `solutions/ch04/ex2-benchmark.cpp`

### Exercise 4.3: Perfect Forwarding

Implement a factory function with perfect forwarding:

```cpp
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    // TODO: Use std::forward to perfect-forward args
}

// Test:
struct Point {
    int x, y;
    Point(int x, int y) : x(x), y(y) {}
};

auto p = make_unique<Point>(10, 20);
assert(p->x == 10);
```

**Starter code:** `exercises/ch04/ex3-forward.cpp`  
**Solution:** `solutions/ch04/ex3-forward.cpp`

### Exercise 4.4: Build a Movable-Only Type

```cpp
class UniqueFile {
    FILE* file_;
    
public:
    explicit UniqueFile(const char* path);
    ~UniqueFile();
    
    // Delete copy
    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;
    
    // TODO: Implement move
    UniqueFile(UniqueFile&&) noexcept;
    UniqueFile& operator=(UniqueFile&&) noexcept;
    
    FILE* get() const { return file_; }
};

// Test:
UniqueFile f1("test.txt");
UniqueFile f2 = std::move(f1);  // OK
// UniqueFile f3 = f2;  // Should not compile
```

**Starter code:** `exercises/ch04/ex4-movable-only.cpp`  
**Solution:** `solutions/ch04/ex4-movable-only.cpp`

### Exercise 4.5: Debug Moved-From State

Write a test that demonstrates moved-from state:

```cpp
void test_moved_from() {
    std::string s = "hello world";
    std::string t = std::move(s);
    
    // What is s now?
    // TODO: Test what operations are safe
}
```

**Starter code:** `exercises/ch04/ex5-moved-from.cpp`  
**Solution:** `solutions/ch04/ex5-moved-from.cpp`

---

## Key Takeaways

1. **Lvalues have identity, rvalues are temporary**
   - Lvalue: has address, persists
   - Rvalue: temporary, about to die

2. **Move semantics enable zero-copy transfer**
   - Move constructor swaps pointers
   - 164,000× faster than copy (measured)

3. **std::move is just a cast**
   - Casts to rvalue reference
   - Doesn't move anything itself
   - Enables move constructor to be called

4. **RVO is faster than move**
   - Return local variables by value
   - Compiler constructs in caller's memory
   - Never std::move a return value

5. **Perfect forwarding preserves value category**
   - Use T&& for universal references
   - Use std::forward<T> to preserve lvalue/rvalue
   - Essential for generic wrapper functions

6. **agentty's update() uses move everywhere**
   - Model passed by value (moved in)
   - Returned by value (RVO applies)
   - Zero copies of 28 MB model

7. **Mark move constructors noexcept**
   - Enables std::vector to use move
   - Required for strong exception guarantee

---

## Next Chapter

[Chapter 5: Smart Pointers and Resource Management →](../ch05-smart-pointers/README.md)

In the next chapter, you'll learn:
- std::unique_ptr for exclusive ownership
- std::shared_ptr for shared ownership
- std::weak_ptr for breaking cycles
- When NOT to use smart pointers
- How agentty manages resources without shared_ptr
