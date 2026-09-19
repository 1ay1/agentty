# Section 1.1 Deep Dive: Strong Types and Type Safety

## Learning Outcomes
By the end of this section, you will:
1. **Understand WHY** compile-time type checking prevents real bugs
2. **Know HOW** the strong types pattern works at the compiler level
3. **Build** a complete type-safe ID system from scratch
4. **Explain** the zero runtime cost guarantee
5. **Apply** this pattern to real scenarios with confidence

---

## Part A: The Deep Problem (Runtime vs Compile-Time Type Errors)

### What Happens in Python When Types Go Wrong

```python
# Python code - types are NOT checked until runtime
def save_thread(thread_id: str):
    db.update(f"threads/{thread_id}", {...})
    
def fetch_model_id(request):
    return request.provider_id  # BUG: should return model_id

# Later, in some other file:
model_id = fetch_model_id(request)
save_thread(model_id)  # Python accepts this! No error at this line.
```

**What actually happens:**
1. Code runs without error (Python's type hints are ignored at runtime)
2. Wrong ID (provider_id) is saved to database
3. User selects GPT-4, system loads provider's default model (GPT-3.5)
4. **Silent failure** — no crash, just wrong behavior
5. Hours later: user reports bug, debugging begins

**Why Python can't catch this:**
- Both `thread_id` and `model_id` are just strings at runtime
- Python has no way to distinguish `"provider_abc123"` from `"model_xyz789"`
- Type hints exist only in the code, not in the runtime data

### What Happens in C++ When Types Go Wrong

```cpp
// C++ code - types ARE checked at compile time
void save_thread(ThreadId thread_id) {
    db.update(f"threads/{thread_id.value}", {...});
}

ModelId fetch_model_id(const Request& request) {
    return ModelId{request.model_id};
}

// Later, in some other file:
ModelId model_id = fetch_model_id(request);
save_thread(model_id);  // COMPILE ERROR: expected ThreadId, got ModelId
```

**What happens:**
1. Compiler sees type mismatch
2. **Refuses to compile** the entire program
3. Error message points to exact line
4. Bug is caught before any user sees it
5. Developer fixes it in seconds

**Why C++ can catch this:**
- `ThreadId` and `ModelId` are completely different types at compile time
- Even though both internally hold strings, they are NOT interchangeable
- The compiler enforces this strictly

### The Real Cost Difference

| Scenario | Python | C++ |
|----------|--------|-----|
| Bug introduced | Developer makes typo | Developer makes typo |
| Bug detection | User reports crash 6 hours later | Compiler rejects code immediately |
| Time to fix | 2+ hours (debug, test, deploy) | 30 seconds (fix, compile, done) |
| User impact | 100+ users affected | Zero users affected |
| Security risk | If IDs are leaked, wrong data accessed | Impossible by construction |

**This is why agentty uses strong types everywhere.**

---

## Part B: Understanding the Template Pattern

### The Simplest Possible Strong Type

Let's start with the absolute minimum:

```cpp
// Before: stringly typed (dangerous)
std::string get_thread_id() { return "thread_001"; }
std::string get_model_id() { return "model_xyz"; }

void process(std::string id) {
    // Which kind of ID is this? No idea.
    // Could accidentally call it with wrong ID, compiler allows it.
}
```

**Problem:** `process()` doesn't know what kind of ID it's receiving.

Now with strong types:

```cpp
// After: strongly typed (safe)
struct ThreadId {
    std::string value;
};

struct ModelId {
    std::string value;
};

ThreadId get_thread_id() { return ThreadId{"thread_001"}; }
ModelId get_model_id() { return ModelId{"model_xyz"}; }

void process(ThreadId id) {
    // Compiler guarantees this receives a ThreadId, never ModelId
}
```

**Win:** `process()` now has a contract. Pass the wrong type, compiler rejects it.

### Scaling to Many ID Types

But now we have a problem: we're duplicating code for each type:

```cpp
struct ThreadId { std::string value; };
struct ModelId { std::string value; };
struct MessageId { std::string value; };
struct UserId { std::string value; };
struct SessionId { std::string value; };
// ... and 20 more types, all identical except the name
```

**Solution: Use a template with a unique "tag" type for each ID:**

```cpp
// Generic template
template <typename Tag>
struct Id {
    std::string value;
};

// Create unique IDs by providing different Tag types
struct ThreadIdTag {};
struct ModelIdTag {};
struct MessageIdTag {};

using ThreadId = Id<ThreadIdTag>;
using ModelId = Id<ModelIdTag>;
using MessageId = Id<MessageIdTag>;
```

**How the compiler sees this:**
- `Id<ThreadIdTag>` is a completely different type from `Id<ModelIdTag>`
- Even though both have identical code, they are NOT interchangeable
- Trying to pass `ModelId` to a function expecting `ThreadId` = compile error

**Why this works:**
Each template instantiation creates a new type. The compiler doesn't share code between them at the type level (though it may share object code at the binary level via template instantiation).

### Making It Ergonomic

The basic version works but is clunky:

```cpp
ThreadId tid{std::string{"thread_001"}};  // Verbose
```

Let's improve it:

```cpp
template <typename Tag>
struct Id {
    std::string value;
    
    // Explicit constructor: forces you to use the type name
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    
    // Comparison: automatically generated
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
    
    // Optional: print support
    friend std::ostream& operator<<(std::ostream& os, const Id& id) {
        return os << id.value;
    }
};

struct ThreadIdTag {};
using ThreadId = Id<ThreadIdTag>;
```

**Now usage is cleaner:**
```cpp
ThreadId tid{"thread_001"};  // Much better
if (tid == ThreadId{"thread_001"}) { /* ... */ }
std::cout << tid;  // Prints: thread_001
```

---

## Part C: The Zero-Cost Guarantee (Compiler Level)

A critical question: **Does all this type safety add overhead?**

Answer: **Absolutely not.** The strong types cost zero at runtime.

### How to Verify This

Let's look at compiled code:

```cpp
#include <iostream>
#include <string>

template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
};

struct ThreadIdTag {};
using ThreadId = Id<ThreadIdTag>;

ThreadId get_id() {
    return ThreadId{"abc123"};
}

int main() {
    ThreadId tid = get_id();
    std::cout << tid.value << "\n";
}
```

**When compiled with `clang++ -O2 -S` (generate assembly):**

The compiled output has **no runtime code** that knows about "ThreadIdTag" or "Id" template wrapper. It's identical to:

```cpp
std::string get_id() {
    return std::string{"abc123"};
}
```

**Why?**
- `ThreadId` is just a compile-time wrapper around `std::string`
- After type checking, the compiler "erases" the wrapper
- Runtime code operates on the same underlying `std::string`

**This is called "zero-cost abstraction":**
- Full type safety at compile time
- Zero overhead at runtime
- The abstraction is purely for human benefit

### Common Misconception: "Doesn't this bloat the binary?"

No. Consider:

```cpp
// You might think this creates code bloat:
using ThreadId = Id<ThreadIdTag>;
using ModelId = Id<ModelIdTag>;
using MessageId = Id<MessageIdTag>;

// And then:
void process_thread(ThreadId) { /* ... */ }
void process_model(ModelId) { /* ... */ }
void process_message(MessageId) { /* ... */ }
```

**What actually happens:**
- Compiler instantiates template 3 times (one per type)
- Each instantiation creates identical code
- Linker deduplicates identical code
- Final binary size ≈ same as having written it once

---

## Part D: The Constructor Pattern (Preventing Accidental Conversions)

### Without `explicit`

```cpp
struct Id {
    std::string value;
    Id(std::string s) : value(std::move(s)) {}  // NOT explicit
};

using ThreadId = Id<ThreadIdTag>;

void process(ThreadId tid) { /* ... */ }

// Now this compiles but is dangerous:
process("thread_001");  // String implicitly converts to ThreadId!
```

**The problem:**
- You meant to be explicit about types
- But implicit conversions defeat the purpose
- A developer might accidentally pass a string without noticing

### With `explicit`

```cpp
template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}  // explicit
};

using ThreadId = Id<ThreadIdTag>;

void process(ThreadId tid) { /* ... */ }

// Now:
process(ThreadId{"thread_001"});  // OK: explicit conversion
process("thread_001");             // COMPILE ERROR: no implicit conversion
```

**Why `explicit` matters:**
- Forces developer to write `ThreadId{...}` everywhere
- Makes it visually obvious where IDs are being created
- Prevents silent type mismatches

**Rule of thumb:** Always use `explicit` for single-argument constructors unless you have a specific reason not to.

---

## Part E: Real Misconceptions Students Have

### Misconception 1: "Can't I Just Use Strings and Be Careful?"

**Claim:** If I'm careful about variable names, I don't need strong types.

**Reality:**

```cpp
// Your code, being careful:
std::string thread_id = "thread_001";
std::string model_id = "model_xyz";

void process_thread(const std::string& id) {
    // Looks good, right?
}

// Six months later, another developer uses your code:
std::string user_id = get_user_id();
process_thread(user_id);  // Oops! Wrong type. But it compiles!
```

**The problem:** Careful variable naming is not enforceable. The second developer might not even see your intention. Strings don't carry semantic meaning.

**Strong types solve this:**

```cpp
using ThreadId = Id<ThreadIdTag>;
using UserId = Id<UserIdTag>;

void process_thread(ThreadId id) { /* ... */ }

// Now:
UserId user_id = get_user_id();
process_thread(user_id);  // COMPILE ERROR: clear message to developer
```

### Misconception 2: "Don't Strong Types Add Overhead?"

**Claim:** Wrapping strings in structs must add memory or performance cost.

**Reality:**

```cpp
sizeof(std::string) == sizeof(Id<ThreadIdTag>)  // Always true!
sizeof(ThreadId) == 24 bytes (on 64-bit systems)
```

**Why?** The wrapper struct adds zero overhead:
- `Id` contains only one member: `value`
- `value` is a `std::string`
- No virtual functions, no padding, no hidden fields
- The wrapper is purely compile-time, erased at runtime

**Performance test (you can verify):**
```cpp
// Measure copy performance:
ThreadId tid1{"abc123"};
ThreadId tid2 = tid1;  // Same speed as std::string copy

// Measure function call:
void process(ThreadId tid) { }
process(tid1);  // Same cost as passing std::string
```

### Misconception 3: "How is This Different from typedef?"

**Claim:** Can't I just do `using ThreadId = std::string`?

**Reality: No, and here's why:**

```cpp
// With typedef:
using ThreadId = std::string;
using ModelId = std::string;

ThreadId tid{"thread_001"};
ModelId mid{"model_xyz"};

// These are IDENTICAL:
if (tid == mid) { }  // Compiles! They're both strings!

void process(ThreadId id) { }
process(mid);  // Compiles! No type safety!
```

**With strong types:**
```cpp
using ThreadId = Id<ThreadIdTag>;
using ModelId = Id<ModelIdTag>;

ThreadId tid{"thread_001"};
ModelId mid{"model_xyz"};

// These are DIFFERENT:
if (tid == mid) { }  // COMPILE ERROR: can't compare different ID types

void process(ThreadId id) { }
process(mid);  // COMPILE ERROR: ModelId is not ThreadId
```

**Key difference:**
- `typedef` is just an alias (creates no new type)
- `template <typename Tag> struct Id` creates genuinely different types
- Only the struct approach gives you type safety

---

## Part F: Complete Working Example

Here's a complete, compilable example that demonstrates everything:

**File: `examples/ch01-strong-types.cpp`**

```cpp
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <functional>

// ============================================================================
// 1. Define the generic ID template
// ============================================================================

template <typename Tag>
struct Id {
    std::string value;
    
    explicit Id(std::string s) noexcept 
        : value(std::move(s)) 
    {
        if (value.empty()) {
            throw std::invalid_argument("ID cannot be empty");
        }
    }
    
    // Support comparison
    bool operator==(const Id&) const = default;
    auto operator<=>(const Id&) const = default;
    
    // Support printing
    friend std::ostream& operator<<(std::ostream& os, const Id& id) {
        return os << id.value;
    }
};

// ============================================================================
// 2. Create unique ID types
// ============================================================================

struct ThreadIdTag {};
struct MessageIdTag {};
struct UserIdTag {};

using ThreadId = Id<ThreadIdTag>;
using MessageId = Id<MessageIdTag>;
using UserId = Id<UserIdTag>;

// ============================================================================
// 3. Define domain objects using these types
// ============================================================================

struct Message {
    MessageId id;
    UserId author;
    std::string text;
};

struct Thread {
    ThreadId id;
    UserId creator;
    std::vector<Message> messages;
};

// ============================================================================
// 4. Functions using the types (type-safe contracts)
// ============================================================================

// This function ONLY accepts ThreadId, never MessageId or UserId
void display_thread(const Thread& t) {
    std::cout << "Thread " << t.id << " created by " << t.creator << "\n";
    std::cout << "Messages:\n";
    for (const auto& msg : t.messages) {
        std::cout << "  [" << msg.id << "] from " << msg.author << ": " 
                  << msg.text << "\n";
    }
}

// Add message: requires the correct types
void add_message(Thread& thread, MessageId id, UserId author, std::string text) {
    thread.messages.push_back(Message{
        .id = id,
        .author = author,
        .text = std::move(text)
    });
}

// ============================================================================
// 5. Demonstrate type safety
// ============================================================================

int main() {
    // Create some IDs
    ThreadId tid{"thread_001"};
    UserId uid1{"user_alice"};
    UserId uid2{"user_bob"};
    MessageId mid1{"msg_001"};
    MessageId mid2{"msg_002"};
    
    // Create a thread
    Thread thread{
        .id = tid,
        .creator = uid1,
        .messages = {}
    };
    
    // Add messages using correct types
    add_message(thread, mid1, uid1, "Hello!");
    add_message(thread, mid2, uid2, "Hi Alice!");
    
    // Display
    display_thread(thread);
    
    // ========================================================================
    // UNCOMMENT ANY OF THESE TO SEE COMPILE ERRORS (type safety in action)
    // ========================================================================
    
    // add_message(thread, uid1, mid1, "wrong");  // ERROR: MessageId and UserId swapped
    // add_message(thread, uid1, uid2, "wrong");  // ERROR: MessageId expected, UserId given
    // if (tid == uid1) { }                        // ERROR: can't compare ThreadId and UserId
    // display_thread(tid);                        // ERROR: Thread expected, ThreadId given
    
    return 0;
}
```

**To compile and run:**
```bash
cd /data/data/com.termux/files/home/agentty
mkdir -p examples/ch01
# Copy the above code to examples/ch01/strong-types.cpp
clang++ -std=c++20 -o examples/ch01/strong-types examples/ch01/strong-types.cpp
./examples/ch01/strong-types
```

**Expected output:**
```
Thread thread_001 created by user_alice
Messages:
  [msg_001] from user_alice: Hello!
  [msg_002] from user_bob: Hi Alice!
```

---

## Part G: Exercises with Solutions

### Exercise 1.1.1: Implement a UserId System

**Task:**
Create a system with:
- Three ID types: `UserId`, `SessionId`, `RequestId`
- A simple `User` struct with `UserId` and name
- A `Session` struct with `SessionId`, owning `UserId`
- A function that validates you can't mix ID types

**Starter code:**
```cpp
// TODO: Define UserId, SessionId, RequestId using the Id<Tag> pattern

struct User {
    // TODO: fields using UserId
};

struct Session {
    // TODO: fields using SessionId and UserId
};

void add_to_session(Session& s, /* TODO: accept correct ID types */) {
    // TODO: Add logic
}

int main() {
    UserId uid{"user_123"};
    SessionId sid{"session_456"};
    
    // This should compile:
    Session s{sid, uid};
    add_to_session(s, uid);
    
    // These should NOT compile:
    // Session s{uid, sid};  // Swapped arguments
    // add_to_session(s, sid);  // Wrong ID type
}
```

**Solution approach:**
[See next section for full solution]

### Exercise 1.1.2: Add Hashing Support

**Task:**
Extend the `Id<Tag>` template to support hashing, so you can use it in `std::unordered_map`:

```cpp
std::unordered_map<ThreadId, Thread> threads;
threads[ThreadId{"t1"}] = thread;  // Should work after you add hash support
```

**Hint:** You need to specialize `std::hash` for `Id<Tag>`.

---

## Part H: Mastery Quiz

Answer these without looking back:

1. **Explain in one sentence why compile-time type checking is better than runtime type checking.**

2. **Why must we use `explicit` in the `Id` constructor?**

3. **What is the runtime cost of using `Id<ThreadIdTag>` vs. plain `std::string`?**

4. **Why can't we use `using ThreadId = std::string;` for type safety?**

5. **You have `using ModelId = Id<ModelIdTag>`. How many template instantiations does the compiler create, and what is the size of `sizeof(ModelId)`?**

6. **Write a function `get_thread_by_id(ThreadId id)` that only accepts `ThreadId`, never `ModelId`.**

---

## Key Takeaways for Section 1.1

1. **Types are your first line of defense against bugs**
   - Catch errors at compile time, not at runtime
   - Wrong ID type = compiler error, not silent failure

2. **The `Id<Tag>` pattern is zero-cost**
   - Full type safety
   - Zero runtime overhead
   - Binary size is not affected (linker deduplication)

3. **`explicit` forces intentional conversions**
   - Prevents accidental type mixups
   - Makes code self-documenting

4. **This scales to hundreds of types**
   - One template definition
   - Each type gets full compile-time safety
   - No code duplication

---

## Next: Section 1.2 Deep Dive

Once you've mastered Section 1.1 (completed all exercises, passed the quiz), you're ready for **Values vs. References vs. Pointers**, which builds on this foundation.
