# CHAPTER 1 COMPLETE MASTERY CURRICULUM

## Overview

This is your complete guide to mastering **all of Chapter 1: C++ Basics** at deep, internalized level.

You have four deep-dive sections:
1. **Section 1.1** — Strong Types and Type Safety
2. **Section 1.2** — Values, References, and Pointers
3. **Section 1.3** — const Correctness
4. **Section 1.4** — Type Deduction (auto, decltype)

**Total Time to Complete:** 8-10 hours (reading + exercises + practice)

**Format:**
- Each section has deep explanations, working code, common mistakes, and exercises
- Read each section fully before attempting exercises
- Complete exercises before moving to next section
- Pass mastery quiz at end of each section before advancing
- Final capstone project at the end combines all concepts

---

## Chapter 1 Big Picture

### The Unifying Theme: Compile-Time Safety

Everything in Chapter 1 works toward **one goal: catch bugs at compile time, not at runtime**.

```
Section 1.1 (Strong Types)
  ↓
  Use the right type for the right data
  Types are your first defense

Section 1.2 (Values, References, Pointers)
  ↓
  Choose the right way to refer to data
  Ownership and borrowing rules

Section 1.3 (const Correctness)
  ↓
  Make immutability explicit
  Compiler enforces it

Section 1.4 (Type Deduction)
  ↓
  Let the compiler help you
  But be intentional about it
```

### How They Interact

```cpp
// Strong Type (1.1)
using ThreadId = Id<ThreadIdTag>;

// References and const (1.2 + 1.3)
void process(const Thread& thread);

// Type deduction (1.4)
const auto& t = load_thread();
process(t);  // Type checking from 1.1, borrowing from 1.2, const from 1.3
```

---

## Section-by-Section Learning Path

### Section 1.1: Strong Types and Type Safety

**What You'll Learn:**
- Why `ThreadId` ≠ `ModelId` even if both hold strings
- How the template pattern works at compile time
- Why it costs zero at runtime
- How to build type-safe ID systems

**Time:** ~2 hours (reading + exercises)

**Completion Criteria:**
- [ ] Read SECTION_1_1_DEEP_DIVE.md completely
- [ ] Understand why strong types prevent bugs
- [ ] Complete Exercise 1.1.1 (UserId System)
- [ ] Complete Exercise 1.1.2 (Hashing Support)
- [ ] Pass Mastery Quiz (all 6 questions)

**Key Insight:**
> Types are a form of documentation that the compiler enforces. You cannot accidentally use the wrong type.

**Next:** Proceed to Section 1.2

---

### Section 1.2: Values, References, and Pointers

**What You'll Learn:**
- How data ownership works in C++
- When to use values, const refs, mutable refs, and pointers
- Lifetime rules and why the compiler enforces them
- How to avoid dangling references and null pointer bugs

**Time:** ~2.5 hours (reading + exercises)

**Completion Criteria:**
- [ ] Read SECTION_1_2_DEEP_DIVE.md completely
- [ ] Understand the lifetime rules
- [ ] Complete Exercise 1.2.1 (Redesign with References)
- [ ] Complete Exercise 1.2.2 (Implementing Getters)
- [ ] Complete Exercise 1.2.3 (When to Use Each Type)
- [ ] Pass Mastery Quiz (all 6 questions)

**Key Insight:**
> References borrow without owning. Values own. Pointers are for nullable cases. Match the semantic intent to the type.

**Next:** Proceed to Section 1.3

---

### Section 1.3: const Correctness

**What You'll Learn:**
- What `const` actually is (compile-time promise, not runtime check)
- How to mark member functions `const`
- Why const-correctness propagates through code
- How to provide both const and mutable versions of getters

**Time:** ~2 hours (reading + exercises)

**Completion Criteria:**
- [ ] Read SECTION_1_3_DEEP_DIVE.md completely
- [ ] Understand const propagation
- [ ] Complete Exercise 1.3.1 (Add Const Correctness)
- [ ] Complete Exercise 1.3.2 (Const Propagation)
- [ ] Complete Exercise 1.3.3 (Const and Non-Const Overloads)
- [ ] Pass Mastery Quiz (all 6 questions)

**Key Insight:**
> const makes intent explicit: "I will not modify this." The compiler enforces it, preventing accidental mutations.

**Next:** Proceed to Section 1.4

---

### Section 1.4: Type Deduction

**What You'll Learn:**
- How `auto` deduction works (and its gotchas)
- When `auto` saves typing and when it hides intent
- How to use `auto&` and `const auto&` safely
- What `decltype` is for (rare, but useful)
- Structured bindings for clean code

**Time:** ~1.5 hours (reading + exercises)

**Completion Criteria:**
- [ ] Read SECTION_1_4_DEEP_DIVE.md completely
- [ ] Understand auto deduction rules
- [ ] Complete Exercise 1.4.1 (Deduce That Type)
- [ ] Complete Exercise 1.4.2 (When to Use auto vs Explicit)
- [ ] Complete Exercise 1.4.3 (Structured Bindings)
- [ ] Complete Exercise 1.4.4 (Auto and References)
- [ ] Pass Mastery Quiz (all 6 questions)

**Key Insight:**
> auto is a convenience, not magic. Use it for obvious types (iterators, lambdas). Use explicit types for intent.

**Next:** Proceed to Capstone Project

---

## Capstone Project: Build a Type-Safe Configuration System

This project synthesizes all four sections.

### Specification

Build a configuration system for a chat application that demonstrates:

1. **Strong Types (Section 1.1)**
   - `ModelId`, `UserId`, `SessionId` should be incompatible types
   - Cannot accidentally pass wrong ID to a function

2. **Values, References, Pointers (Section 1.2)**
   - Config loads into a value (ownership)
   - Functions borrow const references (borrowing)
   - Optional settings use pointers or `std::optional`

3. **const Correctness (Section 1.3)**
   - Config is immutable after loading
   - Accessor functions are const member functions
   - No accidental mutations possible

4. **Type Deduction (Section 1.4)**
   - Use `auto` appropriately in implementation
   - No unnecessary type verbosity

### Starter Code

```cpp
#include <iostream>
#include <string>
#include <optional>
#include <vector>
#include <map>

// ============================================================================
// Part 1: Strong Types (Section 1.1)
// ============================================================================

template <typename Tag>
struct Id {
    std::string value;
    explicit Id(std::string s) noexcept : value(std::move(s)) {}
    bool operator==(const Id&) const = default;
};

// TODO: Define ModelIdTag, UserIdTag, SessionIdTag
// TODO: Define using declarations

// ============================================================================
// Part 2: Domain Types
// ============================================================================

// TODO: Define Settings struct with fields:
// - model_id (ModelId)
// - timeout_ms (int)
// - max_retries (int)
// - api_key (std::optional<std::string>)

// TODO: Define User struct with:
// - user_id (UserId)
// - name (std::string)
// - settings (Settings)

// TODO: Define Session struct with:
// - session_id (SessionId)
// - user (const User&)  -- should Session own User, or borrow?
// - active (bool)

// ============================================================================
// Part 3: Configuration Manager (Section 1.3 - const correctness)
// ============================================================================

class ConfigManager {
    // TODO: Add private member variables
    
public:
    // Load configuration from file (or simulated)
    static ConfigManager load();  // Returns by value (ownership)
    
    // TODO: Accessors (const member functions returning const references)
    // const Settings& settings() const { /* ... */ }
    // const std::vector<User>& users() const { /* ... */ }
    
    // TODO: Type-safe queries
    // const User* find_user(UserId id) const { /* ... */ }
    // const User* find_user_by_name(const std::string& name) const { /* ... */ }
};

// ============================================================================
// Part 4: Usage (Type Deduction - Section 1.4)
// ============================================================================

void display_config(const ConfigManager& config) {
    // TODO: Use const auto& to iterate
    // for (const auto& user : config.users()) { /* ... */ }
}

int main() {
    // Load configuration
    auto config = ConfigManager::load();
    
    // Access by type-safe ID
    // TODO: Create a UserId
    // auto user = config.find_user(uid);
    
    // Display
    display_config(config);
    
    return 0;
}
```

### Requirements

1. **Compile without errors**
2. **No accidental type mixups** (try to pass ModelId where UserId expected → compile error)
3. **No unnecessary copies** (use const references throughout)
4. **Const-correct** (functions that don't modify are const)
5. **Appropriate auto usage** (iterator types and obvious deductions)

### Grading Criteria

- [ ] Code compiles with `-std=c++20 -Wall -Werror`
- [ ] Type safety: attempting to mix ID types results in compile error
- [ ] Performance: no unnecessary copies (const refs used appropriately)
- [ ] Const correctness: all non-mutating functions are const
- [ ] Code clarity: no over-use of auto where intent is unclear

### Solution Approach

See `CAPSTONE_SOLUTION.md` after completing.

---

## Recommended Reading Order

**Total Time: 8-10 hours**

| Time | Activity | Duration |
|------|----------|----------|
| 0:00-2:00 | Read Section 1.1 + do exercises | 2 hours |
| 2:00-2:15 | Break | 15 min |
| 2:15-4:45 | Read Section 1.2 + do exercises | 2.5 hours |
| 4:45-5:00 | Break | 15 min |
| 5:00-7:00 | Read Section 1.3 + do exercises | 2 hours |
| 7:00-7:15 | Break | 15 min |
| 7:15-8:45 | Read Section 1.4 + do exercises | 1.5 hours |
| 8:45-9:00 | Break | 15 min |
| 9:00-10:00 | Capstone Project | 1 hour |

---

## Quick Reference: Chapter 1 Concepts at a Glance

### Section 1.1: Strong Types
```cpp
// Create distinct types from same underlying data
template <typename Tag> struct Id { std::string value; };
using ModelId = Id<struct ModelIdTag>;
using UserId = Id<struct UserIdTag>;

// Type safety: compiler prevents mixing
void process_model(ModelId id);
process_model(UserId{"u1"});  // ERROR: cannot convert
```

### Section 1.2: Ownership and Borrowing
```cpp
// Value: ownership
std::string s = "hello";

// Reference: borrowing (no copy)
const std::string& ref = s;  // Const borrow (read-only)
std::string& mut_ref = s;     // Mutable borrow (read-write)

// Pointer: optional borrowing
const std::string* ptr = &s;  // Can be null
```

### Section 1.3: const Correctness
```cpp
class Thread {
    std::vector<Message> messages_;
    
public:
    // Const member function: doesn't modify this
    const std::vector<Message>& messages() const {
        return messages_;
    }
    
    // Const reference parameter
    void process(const Thread& t) {
        // t.messages() returns const ref
    }
};
```

### Section 1.4: Type Deduction
```cpp
auto x = 42;           // x is int
auto& ref = x;         // ref is int&
const auto& cref = x;  // cref is const int&

for (const auto& msg : messages) {  // Typical pattern
    process(msg);
}
```

---

## Appendix: Setting Up Your Environment

### Compile and Run Examples

All examples are designed to compile with `clang++` or `g++` using C++20:

```bash
# Example: compile strong types
clang++ -std=c++20 -Wall -Werror \
    examples/ch01-strong-types.cpp \
    -o strong-types

./strong-types
```

### Building with cmake

If you want to integrate into agentty's build system:

```bash
mkdir build
cd build
cmake -DCMAKE_CXX_STANDARD=20 ..
make
```

---

## Mastery Checklist

### Before Moving to Chapter 2, You Should Be Able To:

**Section 1.1:**
- [ ] Explain why compile-time type checking is better than runtime
- [ ] Implement a strong type using `template <typename Tag> struct Id`
- [ ] Understand that strong types have zero runtime cost
- [ ] Recognize when strong types are needed in a system

**Section 1.2:**
- [ ] Explain the difference between values, references, and pointers
- [ ] Choose the right one for any given scenario
- [ ] Understand lifetime rules and why compiler enforces them
- [ ] Implement const and mutable getters for class members

**Section 1.3:**
- [ ] Explain what `const` means (compile-time promise)
- [ ] Mark member functions const appropriately
- [ ] Understand how const propagates through code
- [ ] Implement both const and non-const overloads

**Section 1.4:**
- [ ] Use `auto` appropriately (and know when NOT to use it)
- [ ] Understand why `const auto&` is the safe iteration pattern
- [ ] Know when to be explicit about types
- [ ] Use structured bindings where appropriate

**Overall:**
- [ ] Understand why each concept exists
- [ ] Apply all four concepts together in real code
- [ ] Recognize violations of these principles
- [ ] Explain the concepts to someone else

---

## Frequently Asked Questions

**Q: Do I need to complete all exercises?**
A: Yes. Exercises are where mastery happens. Reading alone is not enough.

**Q: Can I skip a section?**
A: No. Each section builds on the previous. Skip, and you'll be lost in later chapters.

**Q: How do I know I've mastered a section?**
A: You can complete the capstone project that combines all concepts. If that works, you've mastered the chapter.

**Q: What if I get stuck on an exercise?**
A: There are solution hints in each section. Try without looking first, then read hints.

**Q: How much code will I write?**
A: ~500-1000 lines total (split across 8-10 exercises and 1 capstone).

---

## Next: Chapter 2

Once you've completed:
- [ ] All four sections
- [ ] All exercises
- [ ] All mastery quizzes
- [ ] Capstone project

You're ready for **Chapter 2: Memory Management (RAII and Ownership)**.

In Chapter 2, you'll learn:
- How C++ manages memory automatically (RAII)
- Why it's safer than manual memory management
- Ownership semantics and lifetimes
- The Rule of Zero/Three/Five

See: `../ch02-memory/DEEP_DIVE.md`
