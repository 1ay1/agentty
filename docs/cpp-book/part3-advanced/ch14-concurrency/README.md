# Chapter 14: Concurrency — Threads, Atomics, Lock Hierarchies

**Goal:** Master thread-safe concurrent programming with compile-time and runtime safety.

**Time:** 6-7 hours  
**Prerequisites:** Chapters 1-13

---

## 14.1 std::thread Basics

### Creating and Joining Threads

```cpp
#include <thread>
#include <iostream>

void worker_function(int id) {
    std::cout << "Worker " << id << " running\n";
}

int main() {
    // Create thread
    std::thread t1{worker_function, 1};
    std::thread t2{worker_function, 2};
    
    // Must join or detach before thread destructor
    t1.join();  // Wait for t1 to finish
    t2.join();  // Wait for t2 to finish
    
    return 0;
}
```

**Key rules:**
1. **Always join or detach** — Thread destructor calls `std::terminate()` if still joinable
2. **Move-only** — Threads cannot be copied, only moved
3. **Thread ID** — Each thread has unique `std::thread::id`

### Lambda Captures

```cpp
int main() {
    int shared_data = 0;
    
    // Capture by reference (DANGEROUS!)
    std::thread t1{[&]() {
        shared_data++;  // Race condition!
    }};
    
    // Capture by value (SAFE but copies)
    std::thread t2{[shared_data]() {
        std::cout << shared_data;
    }};
    
    t1.join();
    t2.join();
}
```

### Detached Threads (Use Sparingly)

```cpp
void background_task() {
    // Long-running work
}

int main() {
    std::thread t{background_task};
    t.detach();  // Thread continues after main() returns
    
    // WARNING: If main() exits, detached threads are terminated
    // without cleanup
}
```

**When to detach:**
- Never in application code
- Only in daemon processes
- agentty **never** detaches threads

---

## 14.2 std::mutex and RAII Locks

### The Data Race Problem

```cpp
#include <thread>
#include <vector>

int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i) {
        counter++;  // DATA RACE
    }
}

int main() {
    std::thread t1{increment};
    std::thread t2{increment};
    t1.join();
    t2.join();
    
    std::cout << counter << '\n';  // Expected: 200000, Actual: ???
}
```

**Output:** `137423` (or any random number less than 200000)

**Why?** `counter++` compiles to:
```asm
mov eax, [counter]  ; Read
add eax, 1          ; Increment
mov [counter], eax  ; Write
```

Thread interleaving:
```
T1: mov eax, [counter]  ; eax = 0
T2: mov eax, [counter]  ; eax = 0
T1: add eax, 1          ; eax = 1
T2: add eax, 1          ; eax = 1
T1: mov [counter], eax  ; counter = 1
T2: mov [counter], eax  ; counter = 1
```

**Result:** Two increments, but counter is 1 (lost update).

### Solution: std::mutex

```cpp
#include <mutex>

std::mutex mtx;
int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i) {
        mtx.lock();
        counter++;
        mtx.unlock();
    }
}

int main() {
    std::thread t1{increment};
    std::thread t2{increment};
    t1.join();
    t2.join();
    
    std::cout << counter << '\n';  // Always 200000
}
```

**Problem:** What if an exception is thrown between lock/unlock?

```cpp
mtx.lock();
do_something();  // Might throw!
mtx.unlock();    // Never reached if exception thrown
// DEADLOCK: mutex permanently locked
```

### RAII to the Rescue: std::lock_guard

```cpp
std::mutex mtx;
int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i) {
        std::lock_guard<std::mutex> lock{mtx};
        counter++;
        // Automatic unlock when lock goes out of scope
    }
}
```

**How it works:**

```cpp
template <typename Mutex>
class lock_guard {
    Mutex& m_;
public:
    explicit lock_guard(Mutex& m) : m_(m) {
        m_.lock();
    }
    
    ~lock_guard() {
        m_.unlock();
    }
    
    // Non-copyable, non-movable
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
};
```

### std::unique_lock: More Flexible

```cpp
std::mutex mtx;

void process() {
    std::unique_lock<std::mutex> lock{mtx};
    
    // Do some work
    expensive_operation();
    
    // Temporarily release lock
    lock.unlock();
    
    // Do work that doesn't need lock
    independent_work();
    
    // Re-acquire lock
    lock.lock();
    
    // More protected work
    finalize();
    
    // Automatic unlock on scope exit
}
```

**unique_lock features:**
- Can unlock/relock manually
- Can be moved (not copied)
- Works with condition variables
- Slightly more overhead than lock_guard

### Real Example from agentty: Thread Cache

```cpp
// include/agentty/io/cache.hpp

class ThreadCache {
    std::unordered_map<ThreadId, Thread> cache_;
    std::mutex mutex_;
    
public:
    void insert(ThreadId id, Thread thread) {
        std::lock_guard<std::mutex> lock{mutex_};
        cache_[id] = std::move(thread);
    }
    
    std::optional<Thread> find(ThreadId id) const {
        std::lock_guard<std::mutex> lock{mutex_};
        auto it = cache_.find(id);
        if (it != cache_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock{mutex_};
        cache_.clear();
    }
};
```

---

## 14.3 std::atomic and Memory Ordering

### Atomic Operations

```cpp
#include <atomic>

std::atomic<int> counter{0};

void increment() {
    for (int i = 0; i < 100000; ++i) {
        counter++;  // Atomic increment, no mutex needed
    }
}

int main() {
    std::thread t1{increment};
    std::thread t2{increment};
    t1.join();
    t2.join();
    
    std::cout << counter << '\n';  // Always 200000
}
```

**Key insight:** `std::atomic<T>` makes individual operations atomic, but not sequences of operations.

### Atomic vs. Mutex

```cpp
// With mutex:
std::mutex mtx;
int value = 0;

void set_if_zero(int new_value) {
    std::lock_guard lock{mtx};
    if (value == 0) {
        value = new_value;
    }
}

// With atomic (WRONG):
std::atomic<int> value{0};

void set_if_zero(int new_value) {
    if (value == 0) {  // Read
        value = new_value;  // Write
    }
    // RACE: Another thread could set value between read and write
}

// With atomic (CORRECT):
std::atomic<int> value{0};

void set_if_zero(int new_value) {
    int expected = 0;
    value.compare_exchange_strong(expected, new_value);
    // Atomic: if value == expected, set to new_value
}
```

### Memory Ordering

```cpp
std::atomic<int> data{0};
std::atomic<bool> ready{false};

// Writer thread
void writer() {
    data.store(42, std::memory_order_relaxed);
    ready.store(true, std::memory_order_release);  // Synchronizes with acquire
}

// Reader thread
void reader() {
    while (!ready.load(std::memory_order_acquire));  // Synchronizes with release
    int value = data.load(std::memory_order_relaxed);
    assert(value == 42);  // Guaranteed
}
```

**Memory orders:**

| Order | Meaning | Use Case |
|-------|---------|----------|
| `relaxed` | No synchronization, just atomicity | Counters, stats |
| `acquire` | Reads after this see writes before release | Consumer reads flag |
| `release` | Writes before this visible to acquire reads | Producer sets flag |
| `acq_rel` | Both acquire and release | Read-modify-write |
| `seq_cst` | Sequentially consistent (default, slowest) | When in doubt |

**When to use relaxed:**
```cpp
// Just counting, don't care about order
std::atomic<uint64_t> requests_processed{0};

void handle_request() {
    // ...
    requests_processed.fetch_add(1, std::memory_order_relaxed);
}
```

**When to use acquire/release:**
```cpp
// Publishing data
std::atomic<Data*> data_ptr{nullptr};

// Producer
Data* d = new Data{/* ... */};
data_ptr.store(d, std::memory_order_release);

// Consumer
Data* d = data_ptr.load(std::memory_order_acquire);
if (d) {
    use(*d);  // Guaranteed to see initialized data
}
```

### Real Example from maya: Flag Signaling

```cpp
// maya/src/runtime/background.cpp

class BackgroundQueue {
    std::queue<Task> tasks_;
    std::mutex tasks_mutex_;
    std::atomic<bool> stop_{false};
    
public:
    void stop() {
        stop_.store(true, std::memory_order_relaxed);
    }
    
    void worker_thread() {
        while (!stop_.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock{tasks_mutex_};
            
            if (tasks_.empty()) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            
            Task task = std::move(tasks_.front());
            tasks_.pop();
            lock.unlock();
            
            task.execute();
        }
    }
};
```

---

## 14.4 Lock Hierarchies: Preventing Deadlocks

### The Deadlock Problem

```cpp
std::mutex mutex_a;
std::mutex mutex_b;

// Thread 1
void thread1() {
    std::lock_guard lock_a{mutex_a};
    // ... some work ...
    std::lock_guard lock_b{mutex_b};
    // ... work with both locked ...
}

// Thread 2
void thread2() {
    std::lock_guard lock_b{mutex_b};
    // ... some work ...
    std::lock_guard lock_a{mutex_a};
    // DEADLOCK POSSIBLE
}
```

**Execution:**
```
T1: lock(mutex_a)  ✓
T2: lock(mutex_b)  ✓
T1: lock(mutex_b)  ⏳ (waiting for T2)
T2: lock(mutex_a)  ⏳ (waiting for T1)
DEADLOCK
```

### Solution 1: Lock Ordering

```cpp
// Always lock in the same order
void thread1() {
    std::lock_guard lock_a{mutex_a};  // Always A first
    std::lock_guard lock_b{mutex_b};  // Then B
}

void thread2() {
    std::lock_guard lock_a{mutex_a};  // Always A first
    std::lock_guard lock_b{mutex_b};  // Then B
}
```

**Problem:** Easy to forget, hard to enforce across large codebase.

### Solution 2: std::lock (Simultaneous Locking)

```cpp
void transfer(Account& from, Account& to, int amount) {
    // Lock both mutexes simultaneously (deadlock-free)
    std::lock(from.mutex, to.mutex);
    
    // Adopt the locks into RAII guards
    std::lock_guard lock_from{from.mutex, std::adopt_lock};
    std::lock_guard lock_to{to.mutex, std::adopt_lock};
    
    from.balance -= amount;
    to.balance += amount;
}
```

### Solution 3: Ranked Locks (agentty's Approach)

**The idea:** Each mutex has a compile-time RANK. You can only acquire a lock if its rank is LESS than all currently-held ranks.

```cpp
// include/agentty/util/ranked_lock.hpp

template <int Rank>
class RankedMutex {
    static_assert(Rank >= 0 && Rank < 16);
    
    std::mutex m_;
    
public:
    static constexpr int rank = Rank;
    
    void lock() {
        check_rank<Rank>();  // Runtime check
        m_.lock();
        record_held<Rank>();  // Track that we hold this rank
    }
    
    void unlock() {
        clear_held<Rank>();  // Clear tracking
        m_.unlock();
    }
};

// Thread-local tracking
namespace detail {
    constinit thread_local int held_ranks[16] = {0};
    
    template <int R>
    void check_rank() {
        for (int i = R + 1; i < 16; ++i) {
            if (held_ranks[i] != 0) {
                // Trying to lock rank R while holding rank i > R
                dbglog("deadlock", "rank {} acquired while holding {}", R, i);
                std::terminate();  // LOUD failure
            }
        }
    }
    
    template <int R>
    void record_held() {
        held_ranks[R]++;
    }
    
    template <int R>
    void clear_held() {
        held_ranks[R]--;
    }
}
```

**Usage:**

```cpp
// Define lock hierarchy
using SessionLock = RankedMutex<10>;  // Outer lock
using ThreadLock  = RankedMutex<20>;  // Inner lock

SessionLock session_mutex;
ThreadLock thread_mutex;

void process() {
    std::lock_guard session_lock{session_mutex};  // Rank 10
    // ...
    std::lock_guard thread_lock{thread_mutex};    // Rank 20
    // OK: 20 > 10
}

void bad_process() {
    std::lock_guard thread_lock{thread_mutex};    // Rank 20
    // ...
    std::lock_guard session_lock{session_mutex};  // Rank 10
    // CRASH: Trying to lock rank 10 while holding rank 20
}
```

**Why this works:**

1. **Compile-time documentation** — Rank is in the type
2. **Runtime enforcement** — Thread-local tracking catches violations
3. **Fails fast** — `std::terminate()` instead of silent deadlock

### Real Example from agentty: MCP Server Locks

```cpp
// mcp-cpp/src/server.cpp

class McpServer {
    // Lock hierarchy (lower number = acquired first):
    RankedMutex<10> sessions_lock_;       // List of sessions
    RankedMutex<20> caps_lock_;           // Capability registry
    RankedMutex<30> tools_lock_;          // Tool registry
    
    std::unordered_map<SessionId, Session> sessions_;
    CapabilityRegistry caps_;
    ToolRegistry tools_;
    
public:
    void register_session(SessionId id, Session session) {
        std::lock_guard lock{sessions_lock_};  // Rank 10
        sessions_[id] = std::move(session);
    }
    
    void handle_request(SessionId id, Request req) {
        std::lock_guard sessions_lock{sessions_lock_};  // Rank 10
        Session& session = sessions_.at(id);
        
        // Need to access capabilities
        std::lock_guard caps_lock{caps_lock_};  // Rank 20 (OK: 20 > 10)
        
        auto tools = caps_.tools_for_session(id);
        
        // Process with tools
        std::lock_guard tools_lock{tools_lock_};  // Rank 30 (OK: 30 > 20)
        
        auto result = tools_.execute(req.tool, req.args);
        session.send_response(result);
    }
    
    // This would CRASH at runtime:
    // void bad_function() {
    //     std::lock_guard tools_lock{tools_lock_};  // Rank 30
    //     std::lock_guard sessions_lock{sessions_lock_};  // Rank 10
    //     // TERMINATE: rank 10 < 30 (held)
    // }
};
```

---

## 14.5 Real Example: agentty's RankedMutex

### Complete Implementation

```cpp
// include/agentty/util/ranked_lock.hpp

namespace agentty::util {

template <int Rank>
class RankedMutex {
    static_assert(Rank >= 0 && Rank < 16, "Rank must be in [0, 16)");
    
    std::mutex m_;
    
public:
    static constexpr int rank = Rank;
    
    void lock() {
        // Check that no higher-ranked locks are held
        for (int i = Rank + 1; i < 16; ++i) {
            if (detail::held_ranks[i] > 0) {
                std::ostringstream oss;
                oss << "Lock hierarchy violation: trying to acquire rank "
                    << Rank << " while holding rank " << i;
                dbglog("ranked_mutex", "{}", oss.str());
                std::terminate();
            }
        }
        
        m_.lock();
        detail::held_ranks[Rank]++;
    }
    
    void unlock() {
        detail::held_ranks[Rank]--;
        m_.unlock();
    }
    
    bool try_lock() {
        // Same hierarchy check
        for (int i = Rank + 1; i < 16; ++i) {
            if (detail::held_ranks[i] > 0) {
                return false;  // Don't even try
            }
        }
        
        if (m_.try_lock()) {
            detail::held_ranks[Rank]++;
            return true;
        }
        return false;
    }
};

namespace detail {
    // Thread-local storage for held ranks
    constinit thread_local int held_ranks[16] = {0};
}

} // namespace agentty::util
```

### Compile-Time Rank Checking

```cpp
// Helper to check lock order at compile time (when possible)
template <int OuterRank, int InnerRank>
consteval void assert_lock_order() {
    static_assert(InnerRank > OuterRank, 
                  "Inner lock must have higher rank than outer lock");
}

// Usage:
void nested_locks() {
    RankedMutex<10> outer;
    RankedMutex<20> inner;
    
    assert_lock_order<10, 20>();  // Compiles
    // assert_lock_order<20, 10>();  // Compile error
    
    std::lock_guard outer_lock{outer};
    std::lock_guard inner_lock{inner};
}
```

---

## 14.6 Worker Thread Isolation

### The Problem: Panic Propagation

**Before isolation:**

```cpp
void worker_thread() {
    while (true) {
        Task task = queue.pop();
        task.execute();  // Might throw!
    }
}

int main() {
    std::thread worker{worker_thread};
    worker.join();
    
    // If worker_thread throws, entire process terminates
}
```

### Solution: Isolated Workers

```cpp
// include/agentty/util/isolated_thread.hpp

void run_isolated_detached(std::string where, std::function<void()> body) {
    std::thread([where = std::move(where), body = std::move(body)] {
        try {
            body();
        } catch (const std::exception& e) {
            dbglog(where, "worker exception: {}", e.what());
            // Log error, don't terminate process
        } catch (...) {
            dbglog(where, "worker unknown exception");
        }
        // Thread exits cleanly
    }).detach();
}

// Usage:
run_isolated_detached("fetch_worker", [] {
    while (true) {
        auto task = queue.pop();
        task.execute();  // If throws, logged and thread exits
    }
});
```

**Benefits:**
1. **Fault isolation** — One worker crash doesn't kill process
2. **Logging** — Exceptions are logged before thread exits
3. **Graceful degradation** — Other workers continue

### Real Example from agentty: Tool Execution

```cpp
// src/tool/executor.cpp

class ToolExecutor {
    std::atomic<int> active_tools_{0};
    
public:
    void execute_tool(ToolCallId id, std::string name, json args) {
        active_tools_++;
        
        run_isolated_detached("tool_executor", [this, id, name, args = std::move(args)] {
            try {
                auto result = registry_.execute(name, args);
                dispatch(ToolFinished{id, result});
            } catch (const std::exception& e) {
                dbglog("tool_executor", "tool {} failed: {}", name, e.what());
                dispatch(ToolFailed{id, e.what()});
            }
            
            active_tools_--;
        });
    }
    
    void wait_for_all() {
        while (active_tools_.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
};
```

---

## 14.7 Exercises

### Exercise 14.1: Thread-Safe Counter

Implement a thread-safe counter with increment/decrement:

```cpp
class Counter {
public:
    void increment();
    void decrement();
    int get() const;
    
private:
    // TODO: Add mutex and counter
};

// Test with multiple threads
```

**Starter code:** `exercises/ch14/ex1-counter.cpp`  
**Solution:** `solutions/ch14/ex1-counter.cpp`

### Exercise 14.2: Producer-Consumer Queue

Implement a thread-safe queue:

```cpp
template <typename T>
class Queue {
public:
    void push(T value);
    std::optional<T> pop();
    bool empty() const;
    
private:
    // TODO: Add mutex, queue, condition variable
};

// Test with producer/consumer threads
```

**Starter code:** `exercises/ch14/ex2-queue.cpp`  
**Solution:** `solutions/ch14/ex2-queue.cpp`

### Exercise 14.3: Implement RankedMutex

Implement your own version of ranked mutex:

```cpp
template <int Rank>
class MyRankedMutex {
public:
    void lock();
    void unlock();
    bool try_lock();
    
private:
    std::mutex m_;
    // TODO: Thread-local rank tracking
};

// Test that it catches hierarchy violations
```

**Starter code:** `exercises/ch14/ex3-ranked-mutex.cpp`  
**Solution:** `solutions/ch14/ex3-ranked-mutex.cpp`

### Exercise 14.4: Atomic Flag

Implement a spin lock using std::atomic_flag:

```cpp
class SpinLock {
public:
    void lock();
    void unlock();
    
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// Benchmark vs std::mutex
```

**Starter code:** `exercises/ch14/ex4-spinlock.cpp`  
**Solution:** `solutions/ch14/ex4-spinlock.cpp`

### Exercise 14.5: Thread Pool

Implement a simple thread pool:

```cpp
class ThreadPool {
public:
    explicit ThreadPool(int num_threads);
    ~ThreadPool();
    
    void submit(std::function<void()> task);
    void wait_for_all();
    
private:
    // TODO: Worker threads, task queue, synchronization
};

// Test with many tasks
```

**Starter code:** `exercises/ch14/ex5-thread-pool.cpp`  
**Solution:** `solutions/ch14/ex5-thread-pool.cpp`

### Exercise 14.6: Deadlock Detection

Write a test that detects deadlock with RankedMutex:

```cpp
void test_deadlock_detection() {
    RankedMutex<10> a;
    RankedMutex<20> b;
    
    // This should work:
    {
        std::lock_guard lock_a{a};
        std::lock_guard lock_b{b};
    }
    
    // This should terminate:
    try {
        std::lock_guard lock_b{b};
        std::lock_guard lock_a{a};  // CRASH
    } catch (...) {
        // Can't catch std::terminate
    }
}
```

**Starter code:** `exercises/ch14/ex6-deadlock-test.cpp`  
**Solution:** `solutions/ch14/ex6-deadlock-test.cpp`

---

## Key Takeaways

1. **Always use RAII for locks**
   - std::lock_guard for simple cases
   - std::unique_lock for flexibility
   - Never manual lock()/unlock()

2. **Atomics for simple operations**
   - Counters, flags, single values
   - Not a replacement for mutexes
   - Can't make sequences atomic

3. **Memory ordering matters**
   - `relaxed` for counters/stats
   - `acquire`/`release` for synchronization
   - `seq_cst` when in doubt (default)

4. **Lock hierarchies prevent deadlocks**
   - Compile-time rank documentation
   - Runtime enforcement catches violations
   - Better than hoping for lock ordering

5. **Isolate worker threads**
   - Catch all exceptions
   - Log and exit gracefully
   - Don't crash the whole process

6. **agentty's concurrency model**
   - Single-threaded reducer (no locks)
   - Worker threads for I/O
   - Ranked locks where needed
   - Atomic flags for signaling

7. **Testing concurrent code is hard**
   - ThreadSanitizer (TSan)
   - Stress tests with many threads
   - Deliberate bad orderings

---

## Next Chapter

[Chapter 15: Zero-Overhead Abstractions →](../ch15-zero-overhead/README.md)

In the next chapter, you'll learn:
- What "zero-overhead" means
- Inlining and link-time optimization
- Type erasure with std::function
- Small buffer optimization
- How maya achieves zero-allocation rendering
