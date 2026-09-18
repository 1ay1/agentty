# Chapter 2: Memory Management — RAII and Ownership

**Goal:** Master C++'s memory model and resource management.

**Time:** 3-4 hours  
**Prerequisites:** Chapter 1

---

## 2.1 The Stack vs. The Heap

C++ gives you two places to allocate memory:

### The Stack (Automatic Storage)

```cpp
void function() {
    int x = 42;           // Stack: destroyed when function returns
    std::string s = "hi"; // Stack: string object on stack, data on heap
    Thread t = load();    // Stack: destroyed at end of scope
}  // x, s, t all destroyed here automatically
```

**Properties:**
- **Fast** — Just move the stack pointer
- **Automatic** — Destroyed in reverse order of creation
- **Limited** — Typically 1-8 MB per thread
- **LIFO** — Last-In-First-Out

### The Heap (Dynamic Storage)

```cpp
void function() {
    int* p = new int{42};           // Heap: YOU must delete
    Thread* t = new Thread{load()}; // Heap: YOU must delete
    
    // ... use p and t ...
    
    delete p;  // Manual cleanup
    delete t;
}  // If you forget delete = MEMORY LEAK
```

**Properties:**
- **Slower** — malloc/free overhead
- **Manual** — You control lifetime
- **Large** — GBs available
- **Flexible** — Can outlive creating scope

**Modern C++: AVOID raw new/delete**

---

## 2.2 RAII: Resource Acquisition Is Initialization

**The Golden Rule:** Resource lifetime is tied to object lifetime.

### The Problem (C/Old C++)

```cpp
void process_file() {
    FILE* f = fopen("data.txt", "r");
    if (!f) return;  // Early return
    
    char* buffer = malloc(4096);
    if (!buffer) {
        fclose(f);   // Must cleanup manually
        return;
    }
    
    if (parse_fails()) {
        free(buffer);  // Must cleanup manually
        fclose(f);     // Must cleanup manually
        return;
    }
    
    free(buffer);
    fclose(f);
}
// If ANY path forgets cleanup = RESOURCE LEAK
```

### The Solution (RAII)

```cpp
void process_file() {
    std::ifstream f{"data.txt"};
    if (!f) return;  // f's destructor called automatically
    
    std::vector<char> buffer(4096);  // buffer's destructor called automatically
    
    if (parse_fails()) {
        return;  // ALL destructors called automatically
    }
    
    // ALL destructors called automatically
}
```

**Key insight:** The compiler GUARANTEES destructors are called when objects go out of scope (return, exception, normal flow).

### Real Example from agentty: Atomic File Writes

```cpp
// src/io/persistence.cpp
bool write_json_atomic(const fs::path& target, const json& j) {
    fs::path tmp = target;
    tmp += ".tmp";
    
    {  // RAII scope
        std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
        if (!f) return false;
        
        f << j.dump(2);
        f.flush();
        
        #ifndef _WIN32
        // Force to disk before rename
        if (::fsync(fileno(f)) != 0) return false;
        #endif
        
    }  // f's destructor closes file handle
    
    // Atomic rename (old file intact if crash above)
    fs::rename(tmp, target);
    return true;
}
```

**Why RAII matters here:**
1. File handle ALWAYS closed, even if exception thrown
2. No explicit `fclose()` call to forget
3. Clear scope boundaries

### Another Real Example: Ranked Locks

```cpp
// include/agentty/util/ranked_lock.hpp
template <int Rank>
class RankedMutex {
    std::mutex m_;
    
public:
    void lock() {
        check_rank<Rank>();
        m_.lock();
        record_held<Rank>();
    }
    
    void unlock() {
        clear_held<Rank>();
        m_.unlock();
    }
};

// Usage with RAII guard:
void critical_section() {
    RankedMutex<10> mutex;
    
    {
        std::lock_guard<RankedMutex<10>> lock{mutex};
        // Critical section
        // ...
    }  // lock's destructor calls mutex.unlock()
    
    // Mutex automatically released even if exception thrown
}
```

---

## 2.3 Ownership and Lifetimes

**Core concept:** Every resource has EXACTLY ONE owner. When the owner is destroyed, the resource is released.

### Single Ownership Example

```cpp
void process() {
    Thread t = load_thread();  // t OWNS the thread data
    
    modify(t);  // Pass by reference (t still owns)
    
    display(t);  // Pass by reference (t still owns)
    
}  // t destroyed, all thread data freed
```

### Transfer of Ownership (Move Semantics - Chapter 4 preview)

```cpp
Thread create_thread() {
    Thread t;
    t.title = "New Thread";
    return t;  // Ownership transferred to caller (move, not copy)
}

void process() {
    Thread t = create_thread();  // t now owns the data
    
    save_thread(std::move(t));   // Ownership transferred to save_thread
    
    // t is now in "moved-from" state (don't use it)
}
```

### Shared Ownership (Rare, Chapter 5)

```cpp
// When multiple owners need the same data
std::shared_ptr<Thread> t = load_thread();
auto copy1 = t;  // Reference count++
auto copy2 = t;  // Reference count++
// Thread data freed when last shared_ptr destroyed
```

**In agentty: Almost everything is single-ownership**

```cpp
struct Model {
    Thread thread;           // Model OWNS the thread
    std::vector<Message> queue;  // Model OWNS the queued messages
};

// Update transfers ownership:
std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // m is MOVED into this function
    // We modify it
    // We return it (MOVED to caller)
    return {std::move(m), cmd};
}
```

---

## 2.4 Rule of Zero/Three/Five

When defining a class, you have three options:

### Rule of Zero (Preferred)

**If your class manages no resources, don't define any special members.**

```cpp
struct Message {
    MessageId id;
    Role role;
    std::string text;           // Manages its own memory
    std::vector<ImageContent> images;  // Manages its own memory
    
    // NO user-defined destructor, copy, or move
    // Compiler generates correct defaults
};
```

**The compiler generates:**
- Destructor: Calls destructor of each member
- Copy constructor: Calls copy constructor of each member
- Move constructor: Calls move constructor of each member
- Copy assignment: Calls copy assignment of each member
- Move assignment: Calls move assignment of each member

**This is 99% of agentty's classes.**

### Rule of Three (Legacy)

**If you define one of {destructor, copy constructor, copy assignment}, define all three.**

```cpp
class LegacyBuffer {
    char* data_;
    size_t size_;
    
public:
    // Constructor
    LegacyBuffer(size_t n) : data_(new char[n]), size_(n) {}
    
    // Destructor
    ~LegacyBuffer() {
        delete[] data_;
    }
    
    // Copy constructor
    LegacyBuffer(const LegacyBuffer& other) 
        : data_(new char[other.size_]), size_(other.size_) {
        std::memcpy(data_, other.data_, size_);
    }
    
    // Copy assignment
    LegacyBuffer& operator=(const LegacyBuffer& other) {
        if (this != &other) {
            delete[] data_;
            data_ = new char[other.size_];
            size_ = other.size_;
            std::memcpy(data_, other.data_, size_);
        }
        return *this;
    }
};
```

**Modern alternative: Use std::vector**

```cpp
class ModernBuffer {
    std::vector<char> data_;
    
    // Rule of Zero: all special members auto-generated correctly
};
```

### Rule of Five (Modern)

**If you define one of {destructor, copy ops, move ops}, define all five.**

```cpp
class Buffer {
    char* data_;
    size_t size_;
    
public:
    Buffer(size_t n) : data_(new char[n]), size_(n) {}
    
    ~Buffer() { delete[] data_; }
    
    // Copy operations
    Buffer(const Buffer& other);
    Buffer& operator=(const Buffer& other);
    
    // Move operations
    Buffer(Buffer&& other) noexcept 
        : data_(other.data_), size_(other.size_) {
        other.data_ = nullptr;
        other.size_ = 0;
    }
    
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
};
```

**But really, just use std::vector.**

### Real Example from agentty: LazyBytes

```cpp
// include/agentty/domain/lazy_bytes.hpp
class LazyBytes {
    using Source = std::variant<std::monostate, std::string, BlobRef>;
    
    Source              source_;
    mutable std::string bytes_;
    mutable bool        resolved_ = true;
    
    // Rule of Zero: std::variant and std::string handle everything
    // No user-defined destructor, copy, or move needed
    
public:
    // Just provide business logic
    const std::string& bytes() const {
        if (!resolved_) {
            bytes_ = resolver_ ? resolver_(source_) : std::string{};
            resolved_ = true;
        }
        return bytes_;
    }
};
```

**Why Rule of Zero wins:**
- Less code
- Harder to get wrong
- Compiler-generated moves are optimal

---

## 2.5 Common Ownership Patterns in agentty

### Pattern 1: Value Semantics (Most Common)

```cpp
// Domain types are values
struct Thread {
    ThreadId id;
    std::string title;
    std::vector<Message> messages;
};

// Pass by const reference (read-only)
void display(const Thread& t);

// Pass by value + move (transfer ownership)
void save(Thread t);

// Return by value (RVO optimization)
Thread load();
```

### Pattern 2: Non-Copyable Resources

```cpp
// HTTP connection is non-copyable
class Connection {
    int socket_fd_;
    
public:
    // Delete copy operations
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    
    // Allow move operations
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    
    ~Connection() {
        if (socket_fd_ >= 0) close(socket_fd_);
    }
};

// Usage:
Connection conn = connect_to_server();  // OK: move
Connection copy = conn;                 // ERROR: copy deleted
std::vector<Connection> pool;
pool.push_back(std::move(conn));       // OK: moved into vector
```

### Pattern 3: Interior Mutability (Rare)

```cpp
// Cache is logically const but physically mutable
class ThreadCache {
    mutable std::unordered_map<ThreadId, Thread> cache_;
    mutable std::mutex mutex_;
    
public:
    // const function that mutates cache (interior mutability)
    const Thread& get(ThreadId id) const {
        std::lock_guard lock{mutex_};
        if (auto it = cache_.find(id); it != cache_.end()) {
            return it->second;
        }
        // Load and cache
        cache_[id] = load_from_disk(id);
        return cache_[id];
    }
};
```

---

## 2.6 Exercises

### Exercise 2.1: Identify Ownership

For each code snippet, identify who owns what:

```cpp
// Snippet A
void process() {
    Thread t = load();
    modify(t);
    save(t);
}

// Snippet B
Thread* create() {
    Thread* t = new Thread();
    return t;
}

// Snippet C
void process(const Thread& t) {
    display(t);
}
```

**Solution:** See `solutions/ch02/ex1-ownership.md`

### Exercise 2.2: Fix the Memory Leak

```cpp
void process_threads() {
    std::vector<Thread*> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.push_back(new Thread{load(i)});
    }
    
    // Process threads...
    for (Thread* t : threads) {
        process(*t);
    }
    
    // Memory leak! Forgot to delete
}
```

Rewrite using RAII (no raw new/delete).

**Solution:** See `solutions/ch02/ex2-fix-leak.cpp`

### Exercise 2.3: Implement RAII File Handle

Implement a RAII wrapper for a FILE*:

```cpp
class FileHandle {
    FILE* file_;
    
public:
    explicit FileHandle(const char* path, const char* mode);
    ~FileHandle();
    
    // Delete copy, allow move
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) noexcept;
    FileHandle& operator=(FileHandle&&) noexcept;
    
    FILE* get() const { return file_; }
    bool is_open() const { return file_ != nullptr; }
};
```

**Starter code:** `exercises/ch02/ex3-file-handle.cpp`  
**Solution:** `solutions/ch02/ex3-file-handle.cpp`

### Exercise 2.4: Rule of Zero vs. Rule of Five

When should you use Rule of Zero, and when must you use Rule of Five?

Implement two classes:
1. `Buffer` — manages raw memory (Rule of Five)
2. `Message` — uses std::string and std::vector (Rule of Zero)

Prove that both have correct ownership semantics.

**Starter code:** `exercises/ch02/ex4-rules.cpp`  
**Solution:** `solutions/ch02/ex4-rules.cpp`

---

## Key Takeaways

1. **Prefer stack allocation (automatic storage)**
   - Fast
   - Automatic cleanup
   - Clear lifetime

2. **RAII: Tie resource lifetime to object lifetime**
   - No manual cleanup
   - Exception-safe
   - Cannot forget to release

3. **Every resource has exactly one owner**
   - Clear responsibility
   - No double-free or use-after-free
   - Easy to reason about

4. **Rule of Zero is the default**
   - Let compiler generate special members
   - Only write Rule of Five when managing raw resources
   - Even then, prefer wrapping in RAII types

5. **In agentty: 99% Rule of Zero**
   - Domain types are values
   - No raw pointers in application code
   - std::vector, std::string manage memory

---

## Next Chapter

[Chapter 3: Templates — Compile-Time Polymorphism →](../ch03-templates/README.md)

In the next chapter, you'll learn:
- Function and class templates
- Template specialization
- Variadic templates
- How agentty's `Id<Tag>` system works
