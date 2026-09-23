# Modern C++ Mastery: From Zero to agentty/maya Expert

**A Production-Grade C++26 Education**

**Version:** 1.0  
**Date:** 2026-09-08  
**Based on:** agentty v0.9.1 & maya TUI framework (423K LOC)  
**Authors:** Derived from production code by experienced C++ systems engineers  
**Target Audience:** Developers who want to write expert-level modern C++

---

## 🎯 What You'll Learn

By the end of this book, you will:

1. **Master modern C++26** — Every feature used in a real, production codebase
2. **Write zero-overhead abstractions** — Type safety with no runtime cost
3. **Build compile-time proofs** — `constexpr`/`consteval` to prevent entire bug classes
4. **Understand functional architecture** — The Elm Architecture in C++
5. **Optimize at the hardware level** — SIMD, cache-friendly data structures
6. **Design safe concurrent systems** — Lock hierarchies, worker isolation
7. **Read and write production code** — No more "toy examples"

---

## 📚 Book Structure

### **Part I: Foundations** (Chapters 1-5)
Essential C++ concepts: types, memory, ownership, templates, move semantics.

### **Part II: Modern C++ Patterns** (Chapters 6-10)
Sum types, monadic error handling, pattern matching, concepts, compile-time computation.

### **Part III: Advanced Techniques** (Chapters 11-15)
SIMD programming, template metaprogramming, type-level proofs, concurrency primitives.

### **Part IV: Architecture** (Chapters 16-20)
The Elm Architecture, effect systems, algebraic data types, dependency injection, TUI frameworks.

### **Part V: Case Studies** (Chapters 21-25)
Real implementations from agentty: persistence, provider abstraction, rendering, lazy loading.

---

## 🗺️ Reading Guide

### **Path 1: Complete Beginner → Expert** (Linear)
Read chapters 1-25 in order. Do every exercise. Expect 6-8 weeks full-time.

### **Path 2: Experienced C++ Developer** (Skip foundations)
Start at Chapter 6 (std::variant). Focus on Part II-IV. ~3-4 weeks.

### **Path 3: Architecture-Focused** (Design patterns)
Read: Ch 1-2 (refresher) → Ch 6-10 (modern patterns) → Ch 16-20 (architecture) → Part V (case studies)

### **Path 4: Performance Engineer** (Low-level optimization)
Read: Ch 11 (SIMD) → Ch 15 (zero-overhead) → Ch 23 (SIMD rendering) → Ch 24 (lazy loading)

---

## 📖 Chapter Listing

### Part I: Foundations

- [**Chapter 1:** Types, Values, and References](part1-foundations/ch01-basics/README.md) — *rewritten, 6–10 hours, 12 sections + 13 runnable programs*
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
  - [1.11 Reading what the tools tell you](part1-foundations/ch01-basics/11-reading-the-tools.md)
  - [Exercises (12)](part1-foundations/ch01-basics/exercises.md) · [Quick reference](part1-foundations/ch01-basics/quick-reference.md)

- [**Chapter 2:** Memory Management: RAII and Ownership](part1-foundations/ch02-memory/README.md)
  - 2.1 The Stack vs. The Heap
  - 2.2 RAII: Resource Acquisition Is Initialization
  - 2.3 Ownership and Lifetimes
  - 2.4 Rule of Zero/Three/Five
  - 2.5 Exercises

- [**Chapter 3:** Templates: Compile-Time Polymorphism](part1-foundations/ch03-templates/README.md)
  - 3.1 Function Templates
  - 3.2 Class Templates
  - 3.3 Template Specialization
  - 3.4 Variadic Templates
  - 3.5 Exercises

- [**Chapter 4:** Move Semantics and Perfect Forwarding](part1-foundations/ch04-move/README.md)
  - 4.1 Lvalues and Rvalues
  - 4.2 Move Constructors and Move Assignment
  - 4.3 std::move and std::forward
  - 4.4 Return Value Optimization (RVO)
  - 4.5 Exercises

- [**Chapter 5:** Smart Pointers and Resource Management](part1-foundations/ch05-smart-pointers/README.md)
  - 5.1 std::unique_ptr: Exclusive Ownership
  - 5.2 std::shared_ptr: Shared Ownership
  - 5.3 std::weak_ptr: Breaking Cycles
  - 5.4 When NOT to Use Smart Pointers
  - 5.5 Exercises

### Part II: Modern C++ Patterns

- [**Chapter 6:** std::variant and Sum Types](part2-modern-patterns/ch06-variant/README.md)
  - 6.1 What Are Sum Types?
  - 6.2 std::variant Basics
  - 6.3 Visiting Variants
  - 6.4 Real Example: agentty's Msg System
  - 6.5 Exercises

- [**Chapter 7:** std::expected and Monadic Error Handling](part2-modern-patterns/ch07-expected/README.md)
  - 7.1 Why Not Exceptions?
  - 7.2 std::expected<T, E> Basics
  - 7.3 Monadic Operations: and_then, or_else, transform
  - 7.4 Real Example: maya's Result<T>
  - 7.5 Exercises

- [**Chapter 8:** std::visit and Pattern Matching](part2-modern-patterns/ch08-visit/README.md)
  - 8.1 The Visitor Pattern
  - 8.2 std::visit for Exhaustive Matching
  - 8.3 The overload Helper
  - 8.4 Real Example: agentty's update() Function
  - 8.5 Exercises

- [**Chapter 9:** Concepts and Constraints](part2-modern-patterns/ch09-concepts/README.md)
  - 9.1 What Are Concepts?
  - 9.2 Defining Concepts
  - 9.3 Using Concepts in Templates
  - 9.4 Standard Library Concepts
  - 9.5 Exercises

- [**Chapter 10:** constexpr and Compile-Time Computation](part2-modern-patterns/ch10-constexpr/README.md)
  - 10.1 constexpr Functions
  - 10.2 consteval: Immediate Functions
  - 10.3 constinit: Compile-Time Initialization
  - 10.4 Real Example: agentty's Tool Catalog Proofs
  - 10.5 Exercises

### Part III: Advanced Techniques

- [**Chapter 11:** SIMD Programming: AVX2, AVX-512, NEON](part3-advanced/ch11-simd/README.md)
  - 11.1 What Is SIMD?
  - 11.2 Intrinsics vs. Auto-Vectorization
  - 11.3 AVX2: 256-bit Vectors
  - 11.4 AVX-512: 512-bit Vectors
  - 11.5 ARM NEON
  - 11.6 Real Example: maya's Cell Comparison
  - 11.7 Exercises

- [**Chapter 12:** Template Metaprogramming](part3-advanced/ch12-metaprogramming/README.md)
  - 12.1 Type Traits
  - 12.2 SFINAE: Substitution Failure Is Not An Error
  - 12.3 Tag Dispatch
  - 12.4 Compile-Time Recursion
  - 12.5 Real Example: agentty's Id<Tag> System
  - 12.6 Exercises

- [**Chapter 13:** Type-Level Proofs with consteval](part3-advanced/ch13-proofs/README.md)
  - 13.1 Why Prove at Compile Time?
  - 13.2 Writing consteval Predicates
  - 13.3 static_assert: The Test That Never Skips
  - 13.4 Real Example: Permission Matrix Proofs
  - 13.5 Exercises

- [**Chapter 14:** Concurrency: Threads, Atomics, Lock Hierarchies](part3-advanced/ch14-concurrency/README.md)
  - 14.1 std::thread Basics
  - 14.2 std::mutex and RAII Locks
  - 14.3 std::atomic and Memory Ordering
  - 14.4 Lock Hierarchies (Ranked Locks)
  - 14.5 Real Example: agentty's RankedMutex
  - 14.6 Exercises

- [**Chapter 15:** Zero-Overhead Abstractions](part3-advanced/ch15-zero-overhead/README.md)
  - 15.1 What Does "Zero-Overhead" Mean?
  - 15.2 Inlining and Link-Time Optimization
  - 15.3 Type Erasure with std::function
  - 15.4 Small Buffer Optimization
  - 15.5 Real Example: maya's Element Rendering
  - 15.6 Exercises

### Part IV: Architecture

- [**Chapter 16:** The Elm Architecture in C++](part4-architecture/ch16-elm/README.md)
  - 16.1 What Is The Elm Architecture?
  - 16.2 Model, Msg, Update, View
  - 16.3 Pure Functions for State Transitions
  - 16.4 Real Example: agentty's Core Loop
  - 16.5 Exercises

- [**Chapter 17:** Effect Systems and Pure Functions](part4-architecture/ch17-effects/README.md)
  - 17.1 Side Effects as Data
  - 17.2 Cmd<Msg>: Describing I/O
  - 17.3 Interpreting Effects
  - 17.4 Real Example: maya's Cmd System
  - 17.5 Exercises

- [**Chapter 18:** Algebraic Data Types](part4-architecture/ch18-adt/README.md)
  - 18.1 Product Types (structs)
  - 18.2 Sum Types (variants)
  - 18.3 Recursive Types
  - 18.4 Real Example: agentty's Thread and Message
  - 18.5 Exercises

- [**Chapter 19:** Dependency Injection via Type Erasure](part4-architecture/ch19-di/README.md)
  - 19.1 The Dependency Inversion Principle
  - 19.2 Type Erasure with std::function
  - 19.3 Testing with Fake Implementations
  - 19.4 Real Example: agentty's Deps Seam
  - 19.5 Exercises

- [**Chapter 20:** Building a TUI Framework: maya Deep Dive](part4-architecture/ch20-tui/README.md)
  - 20.1 Terminal I/O Fundamentals
  - 20.2 The Element Tree
  - 20.3 Layout and Flexbox
  - 20.4 Rendering and Diffing
  - 20.5 Real Example: Complete TUI from Scratch
  - 20.6 Exercises

### Part V: Case Studies

- [**Chapter 21:** Case Study: Thread Persistence](part5-case-studies/ch21-thread-persist/README.md)
  - 21.1 The Problem: Slow Thread Loading
  - 21.2 Design: JSONL + Offset Index
  - 21.3 Implementation Details
  - 21.4 Measured Performance
  - 21.5 Lessons Learned

- [**Chapter 22:** Case Study: Provider Abstraction](part5-case-studies/ch22-provider/README.md)
  - 22.1 The Problem: Supporting Multiple AI Providers
  - 22.2 Design: StreamResult Protocol
  - 22.3 Implementation: Anthropic, OpenAI, Ollama
  - 22.4 Error Classification
  - 22.5 Lessons Learned

- [**Chapter 23:** Case Study: SIMD Rendering](part5-case-studies/ch23-simd-render/README.md)
  - 23.1 The Problem: Slow Terminal Rendering
  - 23.2 Design: Packed 64-bit Cells
  - 23.3 Implementation: AVX2/AVX-512/NEON
  - 23.4 Measured Performance
  - 23.5 Lessons Learned

- [**Chapter 24:** Case Study: Lazy Loading and LazyBytes](part5-case-studies/ch24-lazy-load/README.md)
  - 24.1 The Problem: Decoding 17MB of Images on Every Load
  - 24.2 Design: Lazy Materialization
  - 24.3 Implementation: LazyBytes Class
  - 24.4 Correctness Guarantees
  - 24.5 Lessons Learned

- [**Chapter 25:** Case Study: Building Your Own Terminal Agent](part5-case-studies/ch25-build-agent/README.md)
  - 25.1 Architecture Overview
  - 25.2 Implementing the Core Loop
  - 25.3 Adding Tool Support
  - 25.4 Building the UI
  - 25.5 Next Steps

---

## 🛠️ How to Use This Book

### **Exercises**
Each chapter ends with exercises ranging from "fill in the blank" to "implement this subsystem from scratch." Solutions are in the `solutions/` directory.

### **Code Examples**
All code examples compile with:
```bash
g++-14 -std=c++26 -O2 example.cpp -o example
# or
clang++-18 -std=c++26 -O2 example.cpp -o example
```

Full example projects are in `examples/`.

### **References to agentty/maya**
When you see:
> **See in agentty:** `src/runtime/app/update.cpp:42`

You can read the real production code:
```bash
cd /path/to/agentty
grep -n "std::visit" src/runtime/app/update.cpp | head -20
```

### **Prerequisites**
- Basic programming experience (any language)
- Comfort with command line
- A modern C++ compiler (GCC 14+, Clang 18+, MSVC 19.40+)

---

## 📈 Learning Path Flowchart

```
START
  ↓
[Know basic C++?] ──No──> Part I (Ch 1-5)
  ↓ Yes                        ↓
[Know C++17?] ──No──────────> Part II (Ch 6-10)
  ↓ Yes                        ↓
[Know SIMD?] ──No───────────> Part III (Ch 11-15)
  ↓ Yes                        ↓
[Know TEA?] ──No────────────> Part IV (Ch 16-20)
  ↓ Yes                        ↓
Part V (Ch 21-25) ────────────┘
  ↓
EXPERT
```

---

## 🎓 What Makes This Book Different

1. **Real Code** — Every example is from a production system (423K LOC)
2. **No Toy Examples** — No "class Animal { virtual void speak(); }"
3. **Compile-Time First** — Catch bugs at build time, not runtime
4. **Performance Aware** — Zero-overhead abstractions, SIMD, cache-friendly
5. **Functional Style** — Pure functions, algebraic types, effect systems
6. **Complete** — From "what is a type" to "SIMD row diffing with AVX-512"

---

## 📝 License

This book is derived from the agentty and maya codebases (MIT licensed).
Educational use is encouraged. Commercial redistribution requires attribution.

---

## 🚀 Let's Begin

Ready? Start with [Chapter 1: Types, Values, and References](part1-foundations/ch01-basics/README.md).

Or jump to your level:
- Beginner → [Chapter 1](part1-foundations/ch01-basics/README.md)
- Intermediate → [Chapter 6](part2-modern-patterns/ch06-variant/README.md)
- Advanced → [Chapter 11](part3-advanced/ch11-simd/README.md)
- Architect → [Chapter 16](part4-architecture/ch16-elm/README.md)

**Good luck. You're about to become a C++ expert.**
