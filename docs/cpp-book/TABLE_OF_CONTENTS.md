# Modern C++ Mastery: Complete Table of Contents

## Book Structure

This book contains **25 comprehensive chapters** organized into 5 parts, plus exercises and solutions.

---

## Part I: Foundations (5 chapters)

### [Chapter 1: Types, Values, and References](part1-foundations/ch01-basics/README.md)

Rewritten and expanded. Eleven sections, each its own file, each with a
runnable program in `ch01-basics/code/`. 6–10 hours.

- [1.0 The model you need first](part1-foundations/ch01-basics/00-the-model.md)
- [1.1 What a type actually is](part1-foundations/ch01-basics/01-what-is-a-type.md)
- [1.2 Integers lie to you](part1-foundations/ch01-basics/02-integers-lie.md)
- [1.3 Strong types: agentty's `Id<Tag>`](part1-foundations/ch01-basics/03-strong-types.md)
- [1.4 Value categories](part1-foundations/ch01-basics/04-value-categories.md)
- [1.5 Initialisation](part1-foundations/ch01-basics/05-initialisation.md)
- [1.6 References and const](part1-foundations/ch01-basics/06-references-and-const.md)
- [1.7 Lifetime](part1-foundations/ch01-basics/07-lifetime.md)
- [1.8 Copy, move, elision](part1-foundations/ch01-basics/08-copy-move-elision.md)
- [1.9 auto and decltype](part1-foundations/ch01-basics/09-auto-and-decltype.md)
- [1.10 Capstone: rebuild `ImageContent`](part1-foundations/ch01-basics/10-capstone-imagecontent.md)
- [Exercises (8)](part1-foundations/ch01-basics/exercises.md) · [Quick reference](part1-foundations/ch01-basics/quick-reference.md)

### [Chapter 2: Memory Management — RAII and Ownership](part1-foundations/ch02-memory/README.md)
- 2.1 The Stack vs. The Heap
- 2.2 RAII: Resource Acquisition Is Initialization
- 2.3 Ownership and Lifetimes
- 2.4 Rule of Zero/Three/Five
- 2.5 Common Ownership Patterns in agentty
- 2.6 Exercises (4)

### Chapter 3: Templates — Compile-Time Polymorphism
- 3.1 Function Templates
- 3.2 Class Templates
- 3.3 Template Specialization
- 3.4 Variadic Templates
- 3.5 Real Example: Id<Tag> System
- 3.6 Exercises (5)

### Chapter 4: Move Semantics and Perfect Forwarding
- 4.1 Lvalues and Rvalues
- 4.2 Move Constructors and Move Assignment
- 4.3 std::move and std::forward
- 4.4 Return Value Optimization (RVO)
- 4.5 Real Example: agentty's update() Function
- 4.6 Exercises (5)

### Chapter 5: Smart Pointers and Resource Management
- 5.1 std::unique_ptr: Exclusive Ownership
- 5.2 std::shared_ptr: Shared Ownership
- 5.3 std::weak_ptr: Breaking Cycles
- 5.4 When NOT to Use Smart Pointers
- 5.5 Real Example: HTTP Connection Pool
- 5.6 Exercises (4)

---

## Part II: Modern C++ Patterns (5 chapters)

### [Chapter 6: std::variant and Sum Types](part2-modern-patterns/ch06-variant/README.md)
- 6.1 What Are Sum Types?
- 6.2 std::variant Basics
- 6.3 Visiting Variants: Exhaustive Pattern Matching
- 6.4 Real Example: agentty's Msg System
- 6.5 Advanced: std::variant Implementation Details
- 6.6 Exercises (4)

### Chapter 7: std::expected and Monadic Error Handling
- 7.1 Why Not Exceptions?
- 7.2 std::expected<T, E> Basics
- 7.3 Monadic Operations: and_then, or_else, transform
- 7.4 Real Example: maya's Result<T>
- 7.5 Error Classification in agentty
- 7.6 Exercises (5)

### Chapter 8: std::visit and Pattern Matching
- 8.1 The Visitor Pattern
- 8.2 std::visit for Exhaustive Matching
- 8.3 The overload Helper
- 8.4 Real Example: agentty's update() Function
- 8.5 Nested Variants and Performance
- 8.6 Exercises (4)

### Chapter 9: Concepts and Constraints
- 9.1 What Are Concepts?
- 9.2 Defining Concepts
- 9.3 Using Concepts in Templates
- 9.4 Standard Library Concepts
- 9.5 Real Example: agentty's Constrained Types
- 9.6 Exercises (5)

### Chapter 10: constexpr and Compile-Time Computation
- 10.1 constexpr Functions
- 10.2 consteval: Immediate Functions
- 10.3 constinit: Compile-Time Initialization
- 10.4 Real Example: agentty's Tool Catalog Proofs
- 10.5 Compile-Time vs Runtime Tradeoffs
- 10.6 Exercises (6)

---

## Part III: Advanced Techniques (5 chapters)

### [Chapter 11: SIMD Programming — AVX2, AVX-512, NEON](part3-advanced/ch11-simd/README.md)
- 11.1 What Is SIMD?
- 11.2 Intrinsics vs. Auto-Vectorization
- 11.3 AVX2: 256-bit Vectors
- 11.4 AVX-512: 512-bit Vectors
- 11.5 ARM NEON
- 11.6 Real Example: maya's Terminal Cell Comparison
- 11.7 Exercises (4)

### Chapter 12: Template Metaprogramming
- 12.1 Type Traits
- 12.2 SFINAE: Substitution Failure Is Not An Error
- 12.3 Tag Dispatch
- 12.4 Compile-Time Recursion
- 12.5 Real Example: agentty's Id<Tag> System
- 12.6 std::conditional and std::enable_if
- 12.7 Exercises (6)

### Chapter 13: Type-Level Proofs with consteval
- 13.1 Why Prove at Compile Time?
- 13.2 Writing consteval Predicates
- 13.3 static_assert: The Test That Never Skips
- 13.4 Real Example: Permission Matrix Proofs
- 13.5 Proving Invariants: Tool Catalog
- 13.6 Exercises (5)

### Chapter 14: Concurrency — Threads, Atomics, Lock Hierarchies
- 14.1 std::thread Basics
- 14.2 std::mutex and RAII Locks
- 14.3 std::atomic and Memory Ordering
- 14.4 Lock Hierarchies (Ranked Locks)
- 14.5 Real Example: agentty's RankedMutex
- 14.6 Worker Thread Isolation
- 14.7 Exercises (6)

### Chapter 15: Zero-Overhead Abstractions
- 15.1 What Does "Zero-Overhead" Mean?
- 15.2 Inlining and Link-Time Optimization
- 15.3 Type Erasure with std::function
- 15.4 Small Buffer Optimization
- 15.5 Real Example: maya's Element Rendering
- 15.6 Measuring Overhead
- 15.7 Exercises (5)

---

## Part IV: Architecture (5 chapters)

### [Chapter 16: The Elm Architecture in C++](part4-architecture/ch16-elm/README.md)
- 16.1 What Is The Elm Architecture?
- 16.2 Model, Msg, Update, View
- 16.3 Pure Functions for State Transitions
- 16.4 Real Example: agentty's Core Loop
- 16.5 Exercises (4)

### Chapter 17: Effect Systems and Pure Functions
- 17.1 Side Effects as Data
- 17.2 Cmd<Msg>: Describing I/O
- 17.3 Interpreting Effects
- 17.4 Real Example: maya's Cmd System
- 17.5 Task Scheduling and Batching
- 17.6 Exercises (5)

### Chapter 18: Algebraic Data Types
- 18.1 Product Types (structs)
- 18.2 Sum Types (variants)
- 18.3 Recursive Types
- 18.4 Real Example: agentty's Thread and Message
- 18.5 Pattern Matching Exhaustiveness
- 18.6 Exercises (5)

### Chapter 19: Dependency Injection via Type Erasure
- 19.1 The Dependency Inversion Principle
- 19.2 Type Erasure with std::function
- 19.3 Testing with Fake Implementations
- 19.4 Real Example: agentty's Deps Seam
- 19.5 Interface Segregation
- 19.6 Exercises (4)

### Chapter 20: Building a TUI Framework — maya Deep Dive
- 20.1 Terminal I/O Fundamentals
- 20.2 The Element Tree
- 20.3 Layout and Flexbox
- 20.4 Rendering and Diffing
- 20.5 Real Example: Complete TUI from Scratch
- 20.6 Input Handling and Events
- 20.7 Exercises (6)

---

## Part V: Case Studies (5 chapters)

### Chapter 21: Case Study — Thread Persistence
- 21.1 The Problem: Slow Thread Loading
- 21.2 Design: JSONL + Offset Index
- 21.3 Implementation Details
- 21.4 Measured Performance (4-5× speedup)
- 21.5 Lessons Learned

### Chapter 22: Case Study — Provider Abstraction
- 22.1 The Problem: Supporting Multiple AI Providers
- 22.2 Design: StreamResult Protocol
- 22.3 Implementation: Anthropic, OpenAI, Ollama
- 22.4 Error Classification
- 22.5 Capability Discovery
- 22.6 Lessons Learned

### [Chapter 23: Case Study — SIMD Terminal Rendering](part5-case-studies/ch23-simd-render/README.md)
- 23.1 The Problem: Slow Terminal Rendering
- 23.2 Design: Packed Cells + SIMD Diff
- 23.3 Implementation: AVX2/AVX-512/NEON Row Comparison
- 23.4 Measured Performance (9× speedup with AVX-512)
- 23.5 Lessons Learned
- 23.6 Code Walkthrough: The Full Pipeline
- 23.7 Exercises (4)

### Chapter 24: Case Study — Lazy Loading and LazyBytes
- 24.1 The Problem: Decoding 17MB of Images on Every Load
- 24.2 Design: Lazy Materialization
- 24.3 Implementation: LazyBytes Class
- 24.4 Correctness Guarantees
- 24.5 Measured Performance (5× speedup)
- 24.6 Lessons Learned

### Chapter 25: Case Study — Building Your Own Terminal Agent
- 25.1 Architecture Overview
- 25.2 Implementing the Core Loop
- 25.3 Adding Tool Support
- 25.4 Building the UI
- 25.5 Persistence and Threading
- 25.6 Next Steps

---

## Appendices

### Appendix A: C++ Feature Matrix by Version
- C++11, C++14, C++17, C++20, C++23, C++26
- Compiler support table

### Appendix B: agentty Codebase Reference
- File structure
- Key abstractions
- Where to find things

### Appendix C: Performance Optimization Checklist
- Profiling tools
- Common bottlenecks
- Optimization patterns

### Appendix D: Build Systems and Tooling
- CMake best practices
- Compiler flags
- Static analysis tools

### Appendix E: Further Reading
- Books
- Papers
- Codebases to study

---

## Learning Paths

### Beginner Path (Chapters 1-10)
**Time:** 6-8 weeks full-time  
**Prerequisites:** Basic programming

Complete all chapters in order. Do every exercise. Build small projects after each part.

### Intermediate Path (Chapters 6-20)
**Time:** 3-4 weeks  
**Prerequisites:** Comfortable with C++17

Skip Part I. Focus on modern patterns and architecture.

### Advanced Path (Chapters 11-15, 21-25)
**Time:** 2-3 weeks  
**Prerequisites:** Expert C++ developer

Focus on performance optimization and real case studies.

### Architecture Path (Chapters 1-2, 6-10, 16-20, 21-25)
**Time:** 4-5 weeks  
**Prerequisites:** Software architecture interest

Skip low-level chapters. Focus on design patterns and system architecture.

---

## Statistics

- **Total Chapters:** 25
- **Total Exercises:** 120+
- **Code Examples:** 500+
- **Lines of Teaching Code:** 10,000+
- **References to agentty/maya:** 200+
- **Estimated Study Time:** 150-200 hours

---

## How to Use This Book

1. **Read linearly** if new to modern C++
2. **Jump to topics** if experienced
3. **Do the exercises** — they're essential
4. **Study the real code** — every example references agentty/maya source
5. **Build projects** — apply what you learn

---

## Getting Help

- GitHub Issues: Questions about exercises
- Discord: Real-time discussions
- agentty codebase: grep for concepts in action

---

**Ready to become a C++ expert?**

Start: [Chapter 1: Types, Values, and References](part1-foundations/ch01-basics/README.md)

Or jump to your level:
- [Chapter 6: std::variant](part2-modern-patterns/ch06-variant/README.md) (Intermediate)
- [Chapter 11: SIMD](part3-advanced/ch11-simd/README.md) (Advanced)
- [Chapter 16: Elm Architecture](part4-architecture/ch16-elm/README.md) (Architecture)
